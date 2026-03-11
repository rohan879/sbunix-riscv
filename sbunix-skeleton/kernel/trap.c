#include <trap.h>
#include <printk.h>
#include <riscv.h>
#include <proc.h>
#include <vfs.h>
#include <ext2.h>
#include <syscall.h>
#include <uart.h>
#include <plic.h>
#include <kalloc.h>
#include <kmalloc.h>
#include <string.h>
#include <vm.h>
#include <exec.h>
#include <tty.h>
#include <swap.h>

#define TIMER_FREQ 10000000UL  // 10 MHz timebase

/* Goldfish RTC at 0x101000 (QEMU virt machine) */
#define GOLDFISH_RTC_BASE 0x101000UL
#define GOLDFISH_RTC_TIME_LOW  0x00
#define GOLDFISH_RTC_TIME_HIGH 0x04

static unsigned long boot_time;
static unsigned long ticks;
static uint64_t boot_epoch_ns;  /* RTC nanoseconds at boot */
static unsigned char last_key = 0;

static uint64_t ns_to_time_ticks(uint64_t ns) {
    return (ns + 99) / 100;
}

static uint64_t timespec_rel_deadline(uint64_t sec, uint64_t nsec) {
    uint64_t delta = sec * TIMER_FREQ + ns_to_time_ticks(nsec);
    return r_time() + delta;
}

static void rtc_init(void) {
    volatile uint32_t *rtc = (volatile uint32_t *)GOLDFISH_RTC_BASE;
    uint32_t lo = rtc[0];  /* TIME_LOW */
    uint32_t hi = rtc[1];  /* TIME_HIGH */
    boot_epoch_ns = ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* Get current wall-clock time as {sec, nsec} */
static void get_wall_time(int64_t *sec, int64_t *nsec) {
    uint64_t elapsed_ticks = r_time() - boot_time;
    uint64_t elapsed_ns = elapsed_ticks * 100; /* 10MHz -> ns */
    uint64_t now_ns = boot_epoch_ns + elapsed_ns;
    *sec  = (int64_t)(now_ns / 1000000000ULL);
    *nsec = (int64_t)(now_ns % 1000000000ULL);
}

/* ------------------------------------------------------------------ */
/* Kernel pipe table                                                    */
/* ------------------------------------------------------------------ */
#define NPIPES 16
#define PIPE_BUF 4096

struct kpipe {
  int used;
  char buf[PIPE_BUF];
  int rpos;        // next read position
  int wpos;        // next write position
  int write_open;  // number of writers still open
  int read_open;   // number of readers still open
};

static struct kpipe pipes[NPIPES];

static int alloc_pipe(void) {
  for (int i = 0; i < NPIPES; i++) {
    if (!pipes[i].used) {
      pipes[i].used = 1;
      pipes[i].rpos = 0;
      pipes[i].wpos = 0;
      pipes[i].write_open = 1;
      pipes[i].read_open = 1;
      return i;
    }
  }
  return -1;
}

static int pipe_avail(int pid) {
  return pipes[pid].wpos - pipes[pid].rpos;
}

/* ------------------------------------------------------------------ */
/* File descriptor helpers                                              */
/* ------------------------------------------------------------------ */

/* Allocate a free fd slot in current_proc; returns fd or -1 */
static int alloc_fd(void) {
  struct open_file **ftable = proc_ofile(current_proc);
  for (int i = 0; i < NOFILE; i++) {
    if (ftable[i] == NULL)
      return i;
  }
  return -1;
}

/* Shallow copy fd table from src to dst (for clone), incrementing refcounts */
static void copy_ofile(struct proc *dst, struct proc *src) {
  struct open_file **srctab = proc_ofile(src);
  for (int i = 0; i < NOFILE; i++) {
    dst->ofile[i] = srctab[i];
    if (srctab[i]) {
      srctab[i]->refcount++;
      /* bump pipe refcounts too */
      if (srctab[i]->is_pipe) {
        int pid = srctab[i]->pipe_id;
        if (srctab[i]->pipe_read && pid >= 0)
          pipes[pid].read_open++;
        if (srctab[i]->pipe_write && pid >= 0)
          pipes[pid].write_open++;
      }
    }
  }
}

/* Close an open_file: decrement refcount, handle pipe cleanup, free when zero.
 * Also clears the slot in the given proc's ofile[fd] (or shared_ft). */
static void close_ofile(struct proc *p, int fd) {
  struct open_file **ftable = proc_ofile(p);
  struct open_file *f = ftable[fd];
  if (!f) return;
  ftable[fd] = NULL;

  if (f->is_pipe) {
    int pid = f->pipe_id;
    if (f->pipe_read && pid >= 0) {
      pipes[pid].read_open--;
      if (pipes[pid].read_open <= 0 && pipes[pid].write_open <= 0)
        pipes[pid].used = 0;
      /* Wake any sleeping pipe readers/writers */
      for (struct proc *pp = proc; pp < &proc[64]; pp++) {
        if (pp->state == SLEEPING && pp->waiting_for_uart != 2)
          pp->state = RUNNABLE;
      }
    }
    if (f->pipe_write && pid >= 0) {
      pipes[pid].write_open--;
      if (pipes[pid].read_open <= 0 && pipes[pid].write_open <= 0)
        pipes[pid].used = 0;
      for (struct proc *pp = proc; pp < &proc[64]; pp++) {
        if (pp->state == SLEEPING && pp->waiting_for_uart != 2)
          pp->state = RUNNABLE;
      }
    }
  }

  f->refcount--;
  if (f->refcount <= 0) {
    /* Last reference -- call VFS close if applicable */
    if (!f->is_pipe && f->ops && f->ops->close)
      f->ops->close(f->private);
    vfs_free_file(f);
  }
}

/* ------------------------------------------------------------------ */
/* SBI / timer                                                          */
/* ------------------------------------------------------------------ */

static inline void sbi_set_timer(unsigned long stime) {
    register unsigned long a0 asm("a0") = stime;
    register unsigned long a7 asm("a7") = 0;
    asm volatile("ecall" : : "r" (a0), "r" (a7) : "memory");
}

void timer_init(void) {
    boot_time = r_time();
    ticks = 0;
    rtc_init();
    sbi_set_timer(r_time() + TIMER_FREQ);
}

/* Return seconds elapsed since boot (for /proc/uptime) */
unsigned long get_uptime_secs(void) {
    uint64_t elapsed = r_time() - boot_time;
    return (unsigned long)(elapsed / TIMER_FREQ);
}

static void timer_set_next(void) {
    sbi_set_timer(r_time() + TIMER_FREQ);
}

#ifdef TIMER_DISPLAY
static void print_two_digits(unsigned long n) {
    printk("%lu%lu", n / 10, n % 10);
}

static void display_key(void) {
    printk("\033[1;72H");
    if (last_key == 0) {
        printk("  ");
    } else if (last_key <= 0x1F) {
        printk("^%c", last_key + '@');
    } else if (last_key == 0x7F) {
        printk("^?");
    } else {
        printk("%c ", last_key);
    }
}

static void display_time(void) {
    unsigned long elapsed = (r_time() - boot_time) / TIMER_FREQ;
    unsigned long hours = elapsed / 3600;
    unsigned long minutes = (elapsed % 3600) / 60;
    unsigned long seconds = elapsed % 60;

    // Save cursor, move to row 1 col 60
    printk("\033[s\033[1;60H");
    print_two_digits(hours);
    printk(":");
    print_two_digits(minutes);
    printk(":");
    print_two_digits(seconds);
    display_key();
    // Restore cursor
    printk("\033[u");
}
#endif

/* ------------------------------------------------------------------ */
/* UART / interrupt                                                     */
/* ------------------------------------------------------------------ */

static void handle_external_interrupt(void) {
    int irq = plic_claim();
    if (irq == UART_IRQ) {
        int c;
        while ((c = uart_getc()) != -1) {
            tty_input((char)c);
            last_key = (unsigned char)c;
        }
        /* tty_input() calls uart_wakeup() internally when data is ready */
        /* Also wake any process sleeping in wait4 so it can reap zombies.
         * Only wake waitpid sleepers (waiting_for_uart==0); do NOT wake
         * timed sleeps (waiting_for_uart==2) or UART-input sleeps (==1,
         * already handled by uart_wakeup above). */
        for (struct proc *p = proc; p < &proc[64]; p++) {
            if (p->state == SLEEPING && p->waiting_for_uart == 0)
                p->state = RUNNABLE;
        }
        #ifdef TIMER_DISPLAY
        display_time();
        #endif
        plic_complete(irq);
    } else if (irq != 0) {
        plic_complete(irq);
    }
}

static void handle_timer_interrupt(void) {
    ticks++;
    #ifdef TIMER_DISPLAY
    display_time();
    #endif
    timer_set_next();
    /* Wake processes whose timed sleep has expired */
    for (struct proc *p = proc; p < &proc[64]; p++) {
        if (p->state == SLEEPING && p->waiting_for_uart == 2 &&
            r_time() >= p->sleep_until) {
            p->waiting_for_uart = 0;
            p->state = RUNNABLE;
        }
    }
    /* Check for expired alarms and deliver SIGALRM */
    for (struct proc *p = proc; p < &proc[64]; p++) {
        if (p->state != UNUSED && p->state != ZOMBIE &&
            p->alarm_ticks > 0 && ticks >= p->alarm_ticks) {
            p->alarm_ticks = 0;  /* one-shot */
            p->sigpending |= signal_bit(14);  /* SIGALRM = 14 */
            if (p->state == SLEEPING)
                p->state = RUNNABLE;
        }
    }
    /* Preempt current process on timer tick (round-robin) */
    if (current_proc && current_proc->state == RUNNING) {
        current_proc->state = RUNNABLE;
        swtch(&current_proc->context, &scheduler_context);
    }
}

/* ------------------------------------------------------------------ */
/* copy_pagetable_cow: COW-share user pages from src to dst           */
/* ------------------------------------------------------------------ */
static int copy_pagetable_cow(pagetable_t dst, pagetable_t src, uint64_t sz) {
    /* Walk through user virtual address space and COW-share PTE_U pages */
    for (uint64_t va = 0; va < sz; va += PAGE_SIZE) {
        pte_t *pte = walk(src, va, 0);
        if (!pte)
            continue;

        /* Handle swapped-out pages: duplicate the swap slot */
        if (PTE_IS_SWAPPED(*pte)) {
            int old_slot = PTE_SWAP_SLOT(*pte);
            int new_slot = swap_alloc();
            if (new_slot < 0) return -1; /* swap full */

            char *tmp = kalloc();
            if (!tmp) {
                swap_free(new_slot);
                return -1;
            }
            swap_read(old_slot, tmp);
            swap_write(new_slot, tmp);
            kfree(tmp);

            /* Child gets new slot with same flags */
            pte_t child_pte = PTE_SWAP_ENCODE(new_slot, PTE_SWAP_FLAGS(*pte));
            pte_t *cpte = walk(dst, va, 1);
            if (!cpte) {
                swap_free(new_slot);
                return -1;
            }
            *cpte = child_pte;
            continue;
        }

        if (!(*pte & PTE_V))
            continue;
        /* Only share user-accessible pages */
        if (!(*pte & PTE_U))
            continue;

        uint64_t pa = PTE2PA(*pte);
        int flags = PTE_FLAGS(*pte);

        if (flags & PTE_W) {
            /* Writable page: mark read-only + COW in both parent and child */
            *pte = PA2PTE(pa) | ((flags & ~PTE_W) | PTE_COW | PTE_V);
            flags = (flags & ~PTE_W) | PTE_COW;
        }

        /* Share the same physical page */
        page_ref_inc(pa);
        if (mappages(dst, va, PAGE_SIZE, pa, flags) < 0)
            return -1;
    }
    /* Flush TLB since we modified parent PTEs */
    sfence_vma();
    return 0;
}

/* ------------------------------------------------------------------ */
/* cleanup_proc_resources: free or release shared resources on exit      */
/* Called from SYS_exit, signal default-kill, and SYS_kill SIGKILL.     */
/* ------------------------------------------------------------------ */
static void cleanup_proc_resources(struct proc *p) {
    /* ---- Pagetable / VMAs ---- */
    if (p->shared_pt) {
        p->shared_pt->refcount--;
        if (p->shared_pt->refcount == 0) {
            /* Last user: free VMAs (pagetable pages leak is pre-existing) */
            vma_free_all(p);
            kmfree(p->shared_pt, sizeof(struct shared_pagetable));
        } else {
            /* Other threads still using the pagetable; just free our VMAs */
            vma_free_all(p);
        }
        p->shared_pt = NULL;
        /* Point to kernel pagetable so scheduler doesn't use freed pt */
        p->pagetable = kernel_pagetable;
    } else {
        /* Traditional fork: free VMAs as before */
        vma_free_all(p);
    }

    /* ---- File descriptors ---- */
    if (p->shared_ft) {
        p->shared_ft->refcount--;
        if (p->shared_ft->refcount == 0) {
            /* Last user: close all fds in the shared table.
             * close_ofile uses proc_ofile() which returns shared_ft->ofile. */
            for (int i = 0; i < NOFILE; i++) {
                if (p->shared_ft->ofile[i])
                    close_ofile(p, i);
            }
            kmfree(p->shared_ft, sizeof(struct shared_ftable));
        }
        p->shared_ft = NULL;
    } else {
        /* Traditional: close fds in proc's own ofile[] */
        for (int i = 0; i < NOFILE; i++) {
            if (p->ofile[i])
                close_ofile(p, i);
        }
    }

    /* ---- Signal actions ---- */
    if (p->shared_sa) {
        p->shared_sa->refcount--;
        if (p->shared_sa->refcount == 0)
            kmfree(p->shared_sa, sizeof(struct shared_sigactions));
        p->shared_sa = NULL;
    }
    /* Inline sigactions need no freeing */
}

/* ------------------------------------------------------------------ */
/* sys_clone: fork or create a thread in the current process            */
/* a0 = flags (CLONE_VM | CLONE_FILES | CLONE_SIGHAND | SIGCHLD etc.)  */
/* a1 = child stack pointer (0 means inherit parent sp)                 */
/* ------------------------------------------------------------------ */
static pid_t sys_clone(void) {
    uint64_t flags    = current_proc->trapframe->a0;
    uint64_t child_sp = current_proc->trapframe->a1;
    int clone_flags   = (int)(flags & ~0xFFUL); /* mask out low-byte signal */

    /* CLONE_SIGHAND requires CLONE_VM (Linux semantics) */
    if ((clone_flags & CLONE_SIGHAND) && !(clone_flags & CLONE_VM))
        return -22; /* -EINVAL */

    struct proc *p = allocproc();
    if (!p) return -1;

    /* ---- Common path (all modes) ---- */
    memcpy(p->cwd, current_proc->cwd, sizeof(p->cwd));
    p->umask_val = current_proc->umask_val;
    memcpy(p->name, current_proc->name, sizeof(p->name));

    /* Copy trapframe -- child starts after the ecall just like parent */
    memcpy(p->trapframe, current_proc->trapframe, sizeof(struct trapframe));
    p->trapframe->a0 = 0; /* child returns 0 */

    /* Set child stack pointer if caller provided one */
    if (child_sp != 0)
        p->trapframe->sp = child_sp;

    p->parent = current_proc;
    p->pgid = current_proc->pgid;
    p->sid = current_proc->sid;
    p->clone_flags = clone_flags;

    /* Copy stack bounds and mmap_base from parent */
    p->stack_top = current_proc->stack_top;
    p->stack_bottom = current_proc->stack_bottom;
    p->mmap_base = current_proc->mmap_base;
    p->sz = current_proc->sz;

    /* ---- CLONE_VM: share address space ---- */
    if (clone_flags & CLONE_VM) {
        /* Promote parent's pagetable to shared if not already */
        if (!current_proc->shared_pt) {
            struct shared_pagetable *spt =
                (struct shared_pagetable *)kmalloc(sizeof(struct shared_pagetable));
            if (!spt) goto bad;
            spt->pt = current_proc->pagetable;
            spt->refcount = 1;
            current_proc->shared_pt = spt;
            current_proc->clone_flags |= CLONE_VM;
        }
        current_proc->shared_pt->refcount++;
        p->shared_pt = current_proc->shared_pt;
        p->pagetable = p->shared_pt->pt;

        /* Deep copy VMA list (Option A from design doc) */
        vma_dup(current_proc, p);
    } else {
        /* Traditional COW fork: build independent child page table */
        pagetable_t new_pt = uvmcreate();
        if (!new_pt) goto bad;

        /* Identity map kernel RAM and I/O (same as exec) */
        if (mappages(new_pt, 0x80000000, PHYSTOP - 0x80000000, 0x80000000,
                     PTE_R | PTE_W | PTE_X) < 0) goto bad;
        if (mappages(new_pt, 0x10000000, PAGE_SIZE, 0x10000000,
                     PTE_R | PTE_W) < 0) goto bad;
        /* RTC (0x101000) NOT mapped — only used during kernel boot */
        if (mappages(new_pt, 0x2000000, 0x10000, 0x2000000,
                     PTE_R | PTE_W) < 0) goto bad;
        if (mappages(new_pt, 0x0c000000, 0x400000, 0x0c000000,
                     PTE_R | PTE_W) < 0) goto bad;
        if (mappages_super(new_pt, 0x30000000, 0x200000, 0x30000000,
                     PTE_R | PTE_W) < 0) goto bad;
        if (mappages_super(new_pt, 0x40000000, 0x40000000, 0x40000000,
                     PTE_R | PTE_W) < 0) goto bad;

        /* COW-share user pages */
        if (copy_pagetable_cow(new_pt, current_proc->pagetable, MAXVA) < 0)
            goto bad;

        /* Map signal trampoline into child */
        uvmmap_sigtrampoline(new_pt);

        p->pagetable = new_pt;

        /* Deep copy VMA list */
        vma_dup(current_proc, p);
    }

    /* ---- CLONE_FILES: share file descriptor table ---- */
    if (clone_flags & CLONE_FILES) {
        /* Promote parent's ofile[] to shared if not already */
        if (!current_proc->shared_ft) {
            struct shared_ftable *sft =
                (struct shared_ftable *)kmalloc(sizeof(struct shared_ftable));
            if (!sft) goto bad;
            for (int i = 0; i < NOFILE; i++)
                sft->ofile[i] = current_proc->ofile[i];
            sft->refcount = 1;
            current_proc->shared_ft = sft;
            current_proc->clone_flags |= CLONE_FILES;
        }
        current_proc->shared_ft->refcount++;
        p->shared_ft = current_proc->shared_ft;
    } else {
        /* Traditional: copy file descriptors independently */
        copy_ofile(p, current_proc);
    }

    /* ---- CLONE_SIGHAND: share signal handlers ---- */
    if (clone_flags & CLONE_SIGHAND) {
        /* Promote parent's sigactions to shared if not already */
        if (!current_proc->shared_sa) {
            struct shared_sigactions *ssa =
                (struct shared_sigactions *)kmalloc(sizeof(struct shared_sigactions));
            if (!ssa) goto bad;
            memcpy(ssa->actions, current_proc->sigactions,
                   sizeof(ssa->actions));
            ssa->refcount = 1;
            current_proc->shared_sa = ssa;
            current_proc->clone_flags |= CLONE_SIGHAND;
        }
        current_proc->shared_sa->refcount++;
        p->shared_sa = current_proc->shared_sa;
    } else {
        /* Traditional: copy signal actions by value */
        memcpy(p->sigactions, current_proc->sigactions,
               sizeof(p->sigactions));
    }

    /* Child's kernel context: resume at forkret */
    p->context.ra = (uint64_t)forkret;
    p->context.sp = (uint64_t)p->kstack + PAGE_SIZE;

    p->state = RUNNABLE;
    return p->pid;

bad:
    p->state = UNUSED;
    return -1;
}

/* ------------------------------------------------------------------ */
/* sys_wait4: wait for a child to exit                                  */
/* ------------------------------------------------------------------ */
static pid_t sys_wait4(pid_t wpid, int *wstatus, int options) {
    for (;;) {
        int found_child = 0;
        for (struct proc *p = proc; p < &proc[64]; p++) {
            if (p->state == UNUSED) continue;
            if (p->parent != current_proc) continue;
            if (wpid > 0 && p->pid != wpid) continue;
            found_child = 1;
            if (p->state == ZOMBIE) {
                pid_t pid = p->pid;
                if (wstatus) {
                    if (p->killed_by_signal) {
                        /* Signal kill: low 7 bits = signal number */
                        *wstatus = p->killed_by_signal & 0x7f;
                    } else {
                        /* Normal exit: exit code in upper byte, low byte = 0 */
                        *wstatus = (p->exit_status & 0xff) << 8;
                    }
                }
                /* free child resources */
                p->state = UNUSED;
                p->pid = 0;
                p->parent = NULL;
                return pid;
            }
        }
        if (!found_child) return -1;
        /* WNOHANG: don't block */
        if (options & 1) return 0;
        /* Otherwise sleep until a child exits */
        if (current_proc) {
            current_proc->waiting_for_uart = 0;
            current_proc->state = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Path resolution helpers                                              */
/* ------------------------------------------------------------------ */
#include <tar.h>
extern char _tarfs_start[];
extern char _tarfs_end[];

/* Convert an absolute/relative path to the tarfs "./..." format.
 * tarfs uses paths like "./bin/echo", "./etc/rc", etc.
 * Used by execve to look up ELF binaries. */
static void to_tarfs_path(const char *path, char *out) {
    /* Strip leading slash(es) */
    while (*path == '/') path++;
    if (*path == '\0') {
        out[0] = '.'; out[1] = '\0';
        return;
    }
    out[0] = '.';
    out[1] = '/';
    int i = 2;
    while (*path && i < 255) {
        out[i++] = *path++;
    }
    out[i] = '\0';
}

/* Normalize an absolute path in-place: collapse "." and ".." components.
 * Input must already be absolute (start with '/').  Result is written to out. */
static void normalize_path(const char *in, char *out) {
    /* Use a small stack of component offsets into out[] */
    /* We build the result into out directly, tracking segment starts. */
    char tmp[256];
    int tlen = 0;
    const char *p = in;

    /* Skip leading slash — we'll add it back */
    while (*p == '/') p++;

    tmp[tlen++] = '/';
    /* Stack of segment-end positions in tmp (indices of the '/' before each segment) */
    int stack[64];
    int depth = 0;

    while (*p) {
        /* Find end of this component */
        const char *start = p;
        while (*p && *p != '/') p++;
        int clen = (int)(p - start);

        if (clen == 0 || (clen == 1 && start[0] == '.')) {
            /* "." or empty — skip */
        } else if (clen == 2 && start[0] == '.' && start[1] == '.') {
            /* ".." — pop last segment */
            if (depth > 0) {
                tlen = stack[--depth];
            }
        } else {
            /* Normal component — push */
            stack[depth++] = tlen; /* save position before adding '/' + component */
            if (tlen > 1) tmp[tlen++] = '/'; /* add separator (root already has '/') */
            if (tlen + clen < 255) {
                memcpy(tmp + tlen, start, clen);
                tlen += clen;
            }
        }

        /* skip slashes between components */
        while (*p == '/') p++;
    }

    tmp[tlen] = '\0';
    /* Result: if only the root slash remains, that's "/" */
    memcpy(out, tmp, tlen + 1);
}

/* Resolve a path against cwd.  Result in out[256]. */
static void resolve_path(const char *cwd, const char *path, char *out) {
    char raw[512]; /* intermediate buffer before normalization */
    int rlen;

    if (path[0] == '/') {
        /* absolute */
        rlen = strlen(path);
        if (rlen > 511) rlen = 511;
        memcpy(raw, path, rlen);
        raw[rlen] = '\0';
    } else {
        /* relative — prepend cwd */
        int cwdlen = strlen(cwd);
        int pathlen = strlen(path);
        if (cwdlen + 1 + pathlen > 510) {
            out[0] = '/'; out[1] = '\0';
            return;
        }
        memcpy(raw, cwd, cwdlen);
        if (cwdlen == 0 || cwd[cwdlen-1] != '/') {
            raw[cwdlen++] = '/';
        }
        memcpy(raw + cwdlen, path, pathlen);
        raw[cwdlen + pathlen] = '\0';
    }

    /* Normalize: collapse "." and ".." components */
    normalize_path(raw, out);
}

/* Check if a resolved path exists as a directory by trying to open it */
static int vfs_is_dir(const char *path) {
    const char *relpath;
    struct vfs_ops *ops = vfs_lookup_mount(path, &relpath);
    if (!ops) return 0;
    /* Try stat to check if it's a directory */
    uint8_t st[128];
    if (ops->stat(relpath, st) == 0) {
        uint32_t mode;
        memcpy(&mode, st + 16, 4);
        return (mode & 0xF000) == 0x4000; /* S_IFDIR */
    }
    return 0;
}

/* Follow symlinks in a resolved path (up to 8 levels to prevent loops) */
static int follow_symlinks(char *resolved, int maxdepth) {
    if (maxdepth <= 0) return -40; /* ELOOP */

    const char *relpath;
    struct vfs_ops *ops = vfs_lookup_mount(resolved, &relpath);
    if (!ops || !ops->readlink) return 0; /* no readlink = not a symlink-capable fs */

    char target[256];
    int ret = ops->readlink(relpath, target, sizeof(target) - 1);
    if (ret < 0) return 0; /* not a symlink, path is final */
    target[ret] = '\0';

    /* Resolve the symlink target */
    char new_resolved[256];
    if (target[0] == '/') {
        resolve_path("/", target, new_resolved);
    } else {
        /* Relative: resolve from symlink's parent directory */
        char dir[256];
        int rlen = strlen(resolved);
        if (rlen > 255) rlen = 255;
        memcpy(dir, resolved, rlen);
        dir[rlen] = '\0';
        char *last_slash = dir;
        for (char *p = dir; *p; p++)
            if (*p == '/') last_slash = p;
        if (last_slash != dir)
            *(last_slash + 1) = '\0';
        else
            dir[1] = '\0';

        char combined[512];
        int dlen = strlen(dir);
        int tlen = strlen(target);
        if (dlen + tlen >= 510) return -36; /* ENAMETOOLONG */
        memcpy(combined, dir, dlen);
        memcpy(combined + dlen, target, tlen + 1);

        resolve_path("/", combined, new_resolved);
    }

    int nlen = strlen(new_resolved);
    if (nlen > 255) nlen = 255;
    memcpy(resolved, new_resolved, nlen);
    resolved[nlen] = '\0';

    /* Recursively follow in case target is also a symlink */
    return follow_symlinks(resolved, maxdepth - 1);
}

/* ------------------------------------------------------------------ */
/* Signal delivery                                                      */
/* ------------------------------------------------------------------ */

static void deliver_signal(struct trapframe *tf, struct proc *p) {
    /* Find lowest-numbered pending unblocked signal */
    uint64_t deliverable = p->sigpending & ~p->sigmask;
    if (deliverable == 0)
        return;

    int signum = 0;
    for (int i = 1; i < 32; i++) {
        if (deliverable & signal_bit(i)) {
            signum = i;
            break;
        }
    }

    /* Clear from pending */
    p->sigpending &= ~signal_bit(signum);

    struct kern_sigaction *sa = proc_sigactions(p);
    uint64_t handler = sa[signum].handler;

    /* SIG_DFL (0) -- default action */
    if (handler == 0) {
        /* Ignore-class signals */
        if (signum == SIGCHLD || signum == SIGURG || signum == SIGWINCH)
            return;

        /* Continue: resume stopped process (or ignore if running) */
        if (signum == SIGCONT) {
            p->sigpending &= ~(signal_bit(SIGSTOP) | signal_bit(SIGTSTP) |
                               signal_bit(SIGTTIN) | signal_bit(SIGTTOU));
            return;
        }

        /* Stop-class signals: SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU */
        if (signum == SIGSTOP || signum == SIGTSTP ||
            signum == SIGTTIN || signum == SIGTTOU) {
            p->exit_status = (signum << 8) | 0x7f;
            p->state = SLEEPING;
            p->sigpending &= ~signal_bit(SIGCONT);
            if (p->parent) {
                p->parent->sigpending |= signal_bit(SIGCHLD);
                if (p->parent->state == SLEEPING)
                    p->parent->state = RUNNABLE;
            }
            swtch(&p->context, &scheduler_context);
            return;
        }

        /* Default: terminate process */
        p->exit_status = 128 + signum;
        p->killed_by_signal = signum;
        cleanup_proc_resources(p);
        p->state = ZOMBIE;
        if (p->parent) {
            p->parent->sigpending |= signal_bit(SIGCHLD);
            if (p->parent->state == SLEEPING)
                p->parent->state = RUNNABLE;
        }
        swtch(&p->context, &scheduler_context);
        return;
    }

    /* SIG_IGN (1) -- ignore */
    if (handler == 1)
        return;

    /* User handler: save trapframe on user stack, redirect execution */
    uint64_t sp = tf->sp;

    /* Use alternate signal stack if SA_ONSTACK and alt stack is configured */
    if ((sa[signum].flags & 0x08000000) && /* SA_ONSTACK */
        !(p->ss_flags & 2) &&              /* not SS_DISABLE */
        p->ss_sp != 0 && p->ss_size != 0) {
        /* Check if already on alt stack */
        if (sp < p->ss_sp || sp >= p->ss_sp + p->ss_size) {
            sp = p->ss_sp + p->ss_size;
            p->ss_flags |= 1; /* SS_ONSTACK */
        }
    }

    /* Push the saved mask and original trapframe onto user stack */
    sp -= sizeof(struct signal_frame);
    sp &= ~15UL; /* 16-byte align */

    struct signal_frame frame;
    frame.saved_sigmask = p->sigmask;
    memcpy(&frame.saved_tf, tf, sizeof(frame.saved_tf));

    /* Copy signal frame to user stack via copyout */
    if (copyout(p->pagetable, sp, &frame, sizeof(frame)) < 0)
        return; /* can't deliver signal -- just drop it */

    /* Set up trapframe to call handler(signum) with return to trampoline */
    tf->epc = handler;                   /* jump to signal handler */
    tf->a0 = (uint64_t)signum;           /* first arg: signal number */
    tf->sp = sp;                         /* stack points to saved trapframe */
    tf->ra = SIG_TRAMPOLINE_VA;          /* return address -> trampoline -> sigreturn */

    /* Block this signal during handler execution */
    p->sigmask |= sa[signum].mask;
    if (!(sa[signum].flags & 0x40000000)) /* SA_NODEFER */
        p->sigmask |= signal_bit(signum);
}

/* ------------------------------------------------------------------ */
/* Syscall dispatch table                                               */
/* ------------------------------------------------------------------ */

#define SYSCALL_NORETURN ((long)0x5BC411DEAD)
#define NSYSCALLS 501

static long (*syscall_table[NSYSCALLS])(struct trapframe *tf);

/* ---- Individual syscall handler functions ---- */

static long sys_brk_handler(struct trapframe *tf) {
    uint64_t new_brk = tf->a0;
    uint64_t cur_sz  = current_proc->sz;

    if (new_brk == 0) {
        return (long)cur_sz;
    } else if (new_brk < cur_sz) {
        return (long)cur_sz;
    } else {
        uint64_t heap_start = (cur_sz + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t rounded    = (new_brk + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        if (heap_start < rounded) {
            if (uvmalloc(current_proc->pagetable, heap_start, rounded, PTE_R|PTE_W) == 0) {
                return (long)cur_sz;
            }
        }
        current_proc->sz = new_brk;
        return (long)new_brk;
    }
}

static long sys_write_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint64_t ubuf = tf->a1;
    size_t len = (size_t)tf->a2;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;

    if (f && f->is_pipe && f->pipe_write) {
        int pid = f->pipe_id;
        /* Check for broken pipe before writing */
        if (pipes[pid].read_open <= 0) {
            current_proc->sigpending |= signal_bit(SIGPIPE);
            return (long)-32; /* -EPIPE */
        }
        size_t written = 0;
        while (written < len) {
            /* Re-check after sleeping: reader may have closed */
            if (pipes[pid].read_open <= 0) {
                current_proc->sigpending |= signal_bit(SIGPIPE);
                return (written > 0) ? (long)written : (long)-32;
            }
            int avail = PIPE_BUF - pipe_avail(pid);
            if (avail <= 0) {
                current_proc->state = SLEEPING;
                swtch(&current_proc->context, &scheduler_context);
                continue;
            }
            int n = len - written;
            if (n > avail) n = avail;
            char kchunk[512];
            while (n > 0) {
                int chunk = n > (int)sizeof(kchunk) ? (int)sizeof(kchunk) : n;
                if (copyin(current_proc->pagetable, kchunk, ubuf + written, chunk) < 0) {
                    return (written > 0) ? (long)written : (long)-14;
                }
                for (int i = 0; i < chunk; i++) {
                    pipes[pid].buf[pipes[pid].wpos % PIPE_BUF] = kchunk[i];
                    pipes[pid].wpos++;
                }
                written += chunk;
                n -= chunk;
            }
            for (struct proc *p = proc; p < &proc[64]; p++) {
                if (p->state == SLEEPING && p != current_proc &&
                    p->waiting_for_uart != 2)
                    p->state = RUNNABLE;
            }
        }
        return (long)written;
    } else if (f && f->ops && f->ops->write) {
        /* O_APPEND: seek to end of file before each write */
        if ((f->flags & 02000) && f->ops->fstat) {
            uint8_t st[128];
            if (f->ops->fstat(f->private, st) == 0) {
                int64_t fsize;
                memcpy(&fsize, st + 48, 8);
                f->offset = fsize;
            }
        }
        char kbuf[512];
        int64_t total = 0;
        while ((size_t)total < len) {
            uint64_t chunk = len - total;
            if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
            if (copyin(current_proc->pagetable, kbuf, ubuf + total, chunk) < 0) {
                return (total > 0) ? (long)total : (long)-14;
            }
            int64_t ret = f->ops->write(f->private, kbuf, chunk, &f->offset);
            if (ret < 0) { return (total > 0) ? (long)total : (long)ret; }
            total += ret;
            if ((uint64_t)ret < chunk) break;
        }
        return (long)total;
    } else {
        return (long)-1;
    }
}

static long sys_read_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint64_t ubuf = tf->a1;
    size_t len = (size_t)tf->a2;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;

    if (f && f->is_pipe && f->pipe_read) {
        int pid = f->pipe_id;
        size_t read_bytes = 0;
        /* Block until at least one byte is available (or EOF) */
        while (pipe_avail(pid) == 0) {
            if (pipes[pid].write_open == 0) goto pipe_eof;
            current_proc->state = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
        }
        /* Drain all available bytes (up to len) without re-blocking */
        while (read_bytes < len && pipe_avail(pid) > 0) {
            char kc = pipes[pid].buf[pipes[pid].rpos % PIPE_BUF];
            pipes[pid].rpos++;
            if (copyout(current_proc->pagetable, ubuf + read_bytes, &kc, 1) < 0) {
                return (read_bytes > 0) ? (long)read_bytes : (long)-14;
            }
            read_bytes++;
        }
        /* Wake any blocked pipe writers since we freed buffer space */
        for (struct proc *p = proc; p < &proc[64]; p++) {
            if (p->state == SLEEPING && p != current_proc &&
                p->waiting_for_uart != 2)
                p->state = RUNNABLE;
        }
pipe_eof:
        return (long)read_bytes;
    } else if (f && f->ops && f->ops->read) {
        char kbuf[512];
        int64_t total = 0;
        while ((size_t)total < len) {
            uint64_t chunk = len - total;
            if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
            int64_t ret = f->ops->read(f->private, kbuf, chunk, &f->offset);
            if (ret < 0) { return (total > 0) ? (long)total : (long)ret; }
            if (ret == 0) break;
            if (copyout(current_proc->pagetable, ubuf + total, kbuf, ret) < 0) {
                return (total > 0) ? (long)total : (long)-14;
            }
            total += ret;
            if ((uint64_t)ret < chunk) break;
        }
        return (long)total;
    } else {
        return (long)0;
    }
}

static long sys_openat_handler(struct trapframe *tf) {
    int flags = (int)tf->a2;

    char kpath[256];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }

    char resolved[256];
    resolve_path(current_proc->cwd, kpath, resolved);

    /* Follow symlinks unless O_NOFOLLOW */
    if (!(flags & 0400000)) { /* O_NOFOLLOW = 0400000 */
        follow_symlinks(resolved, 8);
    }

    const char *relpath;
    struct vfs_ops *ops = vfs_lookup_mount(resolved, &relpath);
    if (!ops) {
        return (long)-1;
    }

    void *priv = NULL;
    int ret = ops->open(relpath, flags, &priv);
    if (ret < 0) {
        return (long)ret;
    } else {
        /* Reject opening a directory without O_DIRECTORY (EISDIR = -21) */
#define O_DIRECTORY 0200000
        if (!(flags & O_DIRECTORY) && vfs_is_dir(resolved)) {
            if (ops->close) ops->close(priv);
            return (long)-21;
        }
        struct open_file *f = vfs_alloc_file();
        if (!f) {
            if (ops->close) ops->close(priv);
            return (long)-12;
        }
        int fd = alloc_fd();
        if (fd < 0) {
            if (ops->close) ops->close(priv);
            vfs_free_file(f);
            return (long)-1;
        }
        f->ops = ops;
        f->private = priv;
        f->offset = 0;
        f->flags = flags;
        f->is_pipe = 0;
        {
          int plen = strlen(resolved);
          if (plen > 255) plen = 255;
          memcpy(f->path, resolved, plen);
          f->path[plen] = '\0';
        }
        proc_ofile(current_proc)[fd] = f;
        return (long)fd;
    }
}

static long sys_close_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    if (fd >= 0 && fd < NOFILE && proc_ofile(current_proc)[fd]) {
        close_ofile(current_proc, fd);
        return (long)0;
    } else {
        return (long)-1;
    }
}

static long sys_dup3_handler(struct trapframe *tf) {
    int oldfd = (int)tf->a0;
    int newfd = (int)tf->a1;
    struct open_file **ftab = proc_ofile(current_proc);
    if (oldfd < 0 || oldfd >= NOFILE || !ftab[oldfd] ||
        newfd < 0 || newfd >= NOFILE) {
        return (long)-1;
    } else if (oldfd == newfd) {
        return (long)newfd;
    } else {
        if (ftab[newfd])
            close_ofile(current_proc, newfd);
        ftab[newfd] = ftab[oldfd];
        ftab[oldfd]->refcount++;
        if (ftab[oldfd]->is_pipe) {
            int pid = ftab[oldfd]->pipe_id;
            if (pid >= 0) {
                if (ftab[oldfd]->pipe_read)  pipes[pid].read_open++;
                if (ftab[oldfd]->pipe_write) pipes[pid].write_open++;
            }
        }
        return (long)newfd;
    }
}

static long sys_pipe2_handler(struct trapframe *tf) {
    int pipeid = alloc_pipe();
    if (pipeid < 0) {
        return (long)-1;
    } else {
        struct open_file **ftab = proc_ofile(current_proc);
        struct open_file *rf = vfs_alloc_file();
        struct open_file *wf = vfs_alloc_file();
        int rfd = -1, wfd = -1;
        if (rf) rfd = alloc_fd();
        if (rfd >= 0 && rf) {
            rf->is_pipe = 1;
            rf->pipe_read = 1;
            rf->pipe_write = 0;
            rf->pipe_id = pipeid;
            rf->ops = NULL;
            ftab[rfd] = rf;
            if (wf) wfd = alloc_fd();
        }
        if (wfd < 0 || !wf) {
            if (rfd >= 0) {
                ftab[rfd] = NULL;
                vfs_free_file(rf);
            } else if (rf) {
                vfs_free_file(rf);
            }
            if (wf) vfs_free_file(wf);
            pipes[pipeid].used = 0;
            return (long)-1;
        } else {
            wf->is_pipe = 1;
            wf->pipe_read = 0;
            wf->pipe_write = 1;
            wf->pipe_id = pipeid;
            wf->ops = NULL;
            ftab[wfd] = wf;
            int kfds[2] = { rfd, wfd };
            if (copyout(current_proc->pagetable, tf->a0, kfds, sizeof(kfds)) < 0) {
                close_ofile(current_proc, rfd);
                close_ofile(current_proc, wfd);
                return (long)-14;
            }
            return (long)0;
        }
    }
}

static long sys_getcwd_handler(struct trapframe *tf) {
    size_t sz = (size_t)tf->a1;
    const char *cwd = current_proc->cwd;
    if (!cwd[0]) cwd = "/";
    int len = strlen(cwd);
    if (tf->a0 && (size_t)(len + 1) <= sz) {
        if (copyout(current_proc->pagetable, tf->a0, (void *)cwd, len + 1) < 0) {
            return (long)-14;
        }
        return (long)tf->a0; /* return buf pointer */
    } else {
        return (long)0;
    }
}

static long sys_chdir_handler(struct trapframe *tf) {
    char kpath[256];
    if (copyinstr(current_proc->pagetable, kpath, tf->a0, sizeof(kpath)) < 0) {
        return (long)-14;
    }
    char resolved[256];
    resolve_path(current_proc->cwd, kpath, resolved);
    if (vfs_is_dir(resolved)) {
        memcpy(current_proc->cwd, resolved, sizeof(current_proc->cwd));
        return (long)0;
    } else {
        return (long)-1;
    }
}

static long sys_clone_handler(struct trapframe *tf) {
    (void)tf;
    pid_t child = sys_clone();
    tf->a0 = (uint64_t)child;
    return SYSCALL_NORETURN;
}

static long sys_execve_handler(struct trapframe *tf) {
    char kpath[256];
    if (copyinstr(current_proc->pagetable, kpath, tf->a0, sizeof(kpath)) < 0) {
        return (long)-14;
    }

    uint64_t uargv = tf->a1;
    uint64_t uenvp = tf->a2;

    #define EXECVE_MAXARGS  32
    #define EXECVE_MAXENVS  32

    char *kstrbuf = (char *)kalloc();
    if (!kstrbuf) {
        return (long)-1;
    }

    int kargc = 0;
    int kstroff = 0;
    int kstroff_arr[EXECVE_MAXARGS];

    if (uargv) {
        while (kargc < EXECVE_MAXARGS) {
            uint64_t uptr;
            if (copyin(current_proc->pagetable, &uptr, uargv + (uint64_t)kargc * 8, 8) < 0)
                break;
            if (uptr == 0) break;
            char tmpstr[256];
            if (copyinstr(current_proc->pagetable, tmpstr, uptr, sizeof(tmpstr)) < 0)
                break;
            int slen = strlen(tmpstr) + 1;
            if (kstroff + slen > (int)PAGE_SIZE) break;
            kstroff_arr[kargc] = kstroff;
            memcpy(kstrbuf + kstroff, tmpstr, slen);
            kstroff += slen;
            kargc++;
        }
    }

    char *kenvbuf = (char *)kalloc();
    int kenvc = 0;
    int kenvoff = 0;
    int kenvoff_arr[EXECVE_MAXENVS];

    if (kenvbuf && uenvp) {
        while (kenvc < EXECVE_MAXENVS) {
            uint64_t uptr;
            if (copyin(current_proc->pagetable, &uptr, uenvp + (uint64_t)kenvc * 8, 8) < 0)
                break;
            if (uptr == 0) break;
            char tmpstr[256];
            if (copyinstr(current_proc->pagetable, tmpstr, uptr, sizeof(tmpstr)) < 0)
                break;
            int slen = strlen(tmpstr) + 1;
            if (kenvoff + slen > (int)PAGE_SIZE) break;
            kenvoff_arr[kenvc] = kenvoff;
            memcpy(kenvbuf + kenvoff, tmpstr, slen);
            kenvoff += slen;
            kenvc++;
        }
    }

    char resolved[256];
    char tarpath[256];
    resolve_path(current_proc->cwd, kpath, resolved);
    to_tarfs_path(resolved, tarpath);

    int ret = exec(tarpath);
    if (ret < 0) {
        char orig_tarpath[256];
        to_tarfs_path(kpath, orig_tarpath);
        ret = exec(orig_tarpath);
    }
    if (ret < 0) {
        ret = exec(kpath);
    }

    /* Shebang (#!) handling: if exec failed, check if file is a script */
    if (ret < 0) {
        char first_bytes[256];
        int got = 0;
        struct posix_header *th = tar_find_file(_tarfs_start, _tarfs_end, tarpath);
        if (!th) {
            char orig_tp[256];
            to_tarfs_path(kpath, orig_tp);
            th = tar_find_file(_tarfs_start, _tarfs_end, orig_tp);
        }
        if (th) {
            char *data = (char *)th + sizeof(struct posix_header);
            for (int j = 0; j < 255 && data[j] != '\n' && data[j] != '\0'; j++)
                first_bytes[got++] = data[j];
            first_bytes[got] = '\0';
        }
        if (got >= 2 && first_bytes[0] == '#' && first_bytes[1] == '!') {
            /* Parse interpreter path */
            char interp[256];
            int j = 2;
            while (first_bytes[j] == ' ' || first_bytes[j] == '\t') j++;
            int k = 0;
            while (j < got && first_bytes[j] != '\n' && first_bytes[j] != '\r' &&
                   first_bytes[j] != ' ' && first_bytes[j] != '\t' && k < 254)
                interp[k++] = first_bytes[j++];
            interp[k] = '\0';

            if (k > 0) {
                /* Rebuild argv: [interp, script_path, original_args[1:]...] */
                int interp_len = strlen(interp) + 1;
                int script_len = strlen(resolved) + 1;

                /* Rebuild argv: [interp, script_path, old_argv[1:]...] */
                int new_kstroff = 0;
                char *tmpbuf = (char *)kalloc();
                if (tmpbuf) {
                    /* Copy interp */
                    memcpy(tmpbuf, interp, interp_len);
                    int new_offsets[EXECVE_MAXARGS];
                    int new_argc = 0;
                    new_offsets[new_argc++] = 0;
                    new_kstroff = interp_len;

                    /* Copy script path */
                    new_offsets[new_argc++] = new_kstroff;
                    memcpy(tmpbuf + new_kstroff, resolved, script_len);
                    new_kstroff += script_len;

                    /* Copy old_argv[1..] */
                    for (int a = 1; a < kargc && new_argc < EXECVE_MAXARGS; a++) {
                        int slen = strlen(kstrbuf + kstroff_arr[a]) + 1;
                        if (new_kstroff + slen > (int)PAGE_SIZE) break;
                        new_offsets[new_argc] = new_kstroff;
                        memcpy(tmpbuf + new_kstroff, kstrbuf + kstroff_arr[a], slen);
                        new_kstroff += slen;
                        new_argc++;
                    }

                    /* Replace kstrbuf with new argv */
                    memcpy(kstrbuf, tmpbuf, new_kstroff);
                    kargc = new_argc;
                    kstroff = new_kstroff;
                    for (int a = 0; a < kargc; a++)
                        kstroff_arr[a] = new_offsets[a];
                    kfree(tmpbuf);

                    /* Now exec the interpreter */
                    char interp_tarpath[256];
                    to_tarfs_path(interp, interp_tarpath);
                    ret = exec(interp_tarpath);
                }
            }
        }
    }

    if (ret < 0) {
        kfree(kstrbuf);
        if (kenvbuf) kfree(kenvbuf);
        tf->a0 = (uint64_t)-1;
        return SYSCALL_NORETURN;
    }

    {
        struct trapframe *newtf = current_proc->trapframe;

        int argv_ptrs_sz = (kargc + 1) * 8;
        int envp_ptrs_sz = (kenvc + 1) * 8;
        int header       = 8;
        int total        = header + argv_ptrs_sz + envp_ptrs_sz + kstroff + kenvoff;
        total = (total + 15) & ~15;

        uint64_t sp            = MAXVA - 32 - total;
        uint64_t argv_ptr_base = sp + header;
        uint64_t envp_ptr_base = sp + header + argv_ptrs_sz;
        uint64_t argv_str_base = sp + header + argv_ptrs_sz + envp_ptrs_sz;
        uint64_t envp_str_base = argv_str_base + kstroff;

        char *kbuf = (char *)kalloc();
        if (kbuf) {
            int off = 0;
            uint64_t u_argc = (uint64_t)kargc;
            memcpy(kbuf + off, &u_argc, 8); off += 8;

            uint64_t ptrs[EXECVE_MAXARGS > EXECVE_MAXENVS ?
                          EXECVE_MAXARGS + 1 : EXECVE_MAXENVS + 1];
            int str_off2 = 0;
            for (int i = 0; i < kargc; i++) {
                ptrs[i] = argv_str_base + str_off2;
                int slen = strlen(kstrbuf + kstroff_arr[i]) + 1;
                str_off2 += slen;
            }
            ptrs[kargc] = 0;
            memcpy(kbuf + off, ptrs, argv_ptrs_sz);
            off += argv_ptrs_sz;

            str_off2 = 0;
            for (int i = 0; i < kenvc; i++) {
                ptrs[i] = envp_str_base + str_off2;
                int slen = strlen(kenvbuf + kenvoff_arr[i]) + 1;
                str_off2 += slen;
            }
            ptrs[kenvc] = 0;
            memcpy(kbuf + off, ptrs, envp_ptrs_sz);
            off += envp_ptrs_sz;

            if (kstroff > 0) {
                memcpy(kbuf + off, kstrbuf, kstroff);
                off += kstroff;
            }

            if (kenvoff > 0) {
                memcpy(kbuf + off, kenvbuf, kenvoff);
                off += kenvoff;
            }

            copyout(current_proc->pagetable, sp, kbuf, off);
            kfree(kbuf);
        }

        newtf->sp = sp;
        newtf->a0 = (uint64_t)kargc;
        newtf->a1 = argv_ptr_base;
        newtf->a2 = envp_ptr_base;
    }

    kfree(kstrbuf);
    if (kenvbuf) kfree(kenvbuf);

    trap_restore(current_proc->trapframe);
    /* never reached */
    return SYSCALL_NORETURN;
}

static long sys_wait4_handler(struct trapframe *tf) {
    pid_t wpid   = (pid_t)(int)tf->a0;
    int options  = (int)tf->a2;
    int kwstatus = 0;
    pid_t ret = sys_wait4(wpid, tf->a1 ? &kwstatus : NULL, options);
    if (ret > 0 && tf->a1) {
        if (copyout(current_proc->pagetable, tf->a1, &kwstatus, sizeof(kwstatus)) < 0) {
            return (long)-14;
        }
    }
    return (long)ret;
}

static long sys_getpid_handler(struct trapframe *tf) {
    (void)tf;
    return current_proc ? (long)current_proc->pid : 0;
}

static long sys_getppid_handler(struct trapframe *tf) {
    (void)tf;
    if (current_proc && current_proc->parent)
        return (long)current_proc->parent->pid;
    else
        return (long)1;
}

static long sys_getuid_handler(struct trapframe *tf) {
    (void)tf;
    return (long)0;
}

static long sys_getgid_handler(struct trapframe *tf) {
    (void)tf;
    return (long)0;
}

static long sys_kill_handler(struct trapframe *tf) {
    pid_t target_pid = (pid_t)(int)tf->a0;
    int sig = (int)tf->a1;
    if (target_pid <= 0) {
        return (long)-1;
    }
    struct proc *target = NULL;
    for (int i = 0; i < 64; i++) {
        if (proc[i].state != UNUSED && proc[i].pid == target_pid) {
            target = &proc[i];
            break;
        }
    }
    if (!target) {
        return (long)-3; /* -ESRCH */
    } else {
        if (sig == 9) {
            target->exit_status = 128 + 9;
            target->killed_by_signal = 9;
            cleanup_proc_resources(target);
            target->state = ZOMBIE;
            if (target->parent) {
                target->parent->sigpending |= signal_bit(SIGCHLD);
                if (target->parent->state == SLEEPING)
                    target->parent->state = RUNNABLE;
            }
        } else if (signal_is_valid(sig)) {
            target->sigpending |= signal_bit(sig);
            if (target->state == SLEEPING)
                target->state = RUNNABLE;
            /* SIGCONT: also wake stopped processes */
            if (sig == SIGCONT && target->state == SLEEPING) {
                target->state = RUNNABLE;
            }
        }
        return (long)0;
    }
}

static long sys_clock_gettime_handler(struct trapframe *tf) {
    uint64_t ts_uaddr = tf->a1;
    if (ts_uaddr) {
        int64_t sec, nsec;
        get_wall_time(&sec, &nsec);
        struct { int64_t tv_sec; int64_t tv_nsec; } kts;
        kts.tv_sec = sec;
        kts.tv_nsec = nsec;
        if (copyout(current_proc->pagetable, ts_uaddr, &kts, 16) < 0) {
            return (long)-14;
        } else {
            return (long)0;
        }
    } else {
        return (long)-14;
    }
}

static long sys_lseek_handler(struct trapframe *tf) {
    int fd      = (int)tf->a0;
    long offset = (long)tf->a1;
    int whence  = (int)tf->a2;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;
    if (!f || f->is_pipe) {
        return (long)-1;
    } else {
        int64_t cur = f->offset;
        int64_t new_off;
        if (whence == 0)       /* SEEK_SET */ new_off = (int64_t)offset;
        else if (whence == 1)  /* SEEK_CUR */ new_off = cur + (int64_t)offset;
        else if (whence == 2)  /* SEEK_END */ {
            int64_t file_size = 0;
            if (f->ops && f->ops->fstat) {
                uint8_t stbuf[128];
                if (f->ops->fstat(f->private, stbuf) == 0)
                    memcpy(&file_size, stbuf + 48, 8);
            }
            new_off = file_size + (int64_t)offset;
        }
        else { return (long)-1; }
        if (new_off < 0) new_off = 0;
        f->offset = new_off;
        return (long)new_off;
    }
}

static long sys_fstat_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;
    if (!tf->a1 || !f) {
        return (long)-1;
    } else if (f->is_pipe) {
        uint8_t kstat[128];
        memset(kstat, 0, 128);
        uint32_t mode = 0x1000 | 0600;
        uint32_t nlink = 1;
        uint64_t ino = (uint64_t)(f->pipe_id + 1);
        int32_t blksz = 4096;
        memcpy(kstat + 8, &ino, 8);
        memcpy(kstat + 16, &mode, 4);
        memcpy(kstat + 20, &nlink, 4);
        memcpy(kstat + 56, &blksz, 4);
        if (copyout(current_proc->pagetable, tf->a1, kstat, 128) < 0) {
            return (long)-14;
        }
        return (long)0;
    } else if (f->ops && f->ops->fstat) {
        uint8_t kstat[128];
        int ret = f->ops->fstat(f->private, kstat);
        if (ret == 0) {
            if (copyout(current_proc->pagetable, tf->a1, kstat, 128) < 0) {
                return (long)-14;
            }
        }
        return (long)(int64_t)ret;
    } else {
        uint8_t kstat[128];
        memset(kstat, 0, 128);
        uint64_t dev = 1;
        uint64_t ino = (uint64_t)(fd + 1);
        uint32_t mode = 0x81A4;
        uint32_t nlink = 1;
        int32_t blksz = 4096;
        int64_t sec, nsec;
        get_wall_time(&sec, &nsec);
        memcpy(kstat + 0, &dev, 8);
        memcpy(kstat + 8, &ino, 8);
        memcpy(kstat + 16, &mode, 4);
        memcpy(kstat + 20, &nlink, 4);
        memcpy(kstat + 56, &blksz, 4);
        memcpy(kstat + 72, &sec, 8);
        memcpy(kstat + 80, &nsec, 8);
        memcpy(kstat + 88, &sec, 8);
        memcpy(kstat + 96, &nsec, 8);
        memcpy(kstat + 104, &sec, 8);
        memcpy(kstat + 112, &nsec, 8);
        if (copyout(current_proc->pagetable, tf->a1, kstat, 128) < 0) {
            return (long)-14;
        }
        return (long)0;
    }
}

static long sys_newfstatat_handler(struct trapframe *tf) {
    if (!tf->a1 || !tf->a2) {
        return (long)-1;
    }
    char kpath[256];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }
    char resolved[256];
    resolve_path(current_proc->cwd, kpath, resolved);

    /* Follow symlinks unless AT_SYMLINK_NOFOLLOW */
    int at_flags = (int)tf->a3;
    if (!(at_flags & 0x100)) { /* AT_SYMLINK_NOFOLLOW = 0x100 */
        follow_symlinks(resolved, 8);
    }

    const char *relpath;
    struct vfs_ops *ops = vfs_lookup_mount(resolved, &relpath);
    if (ops && ops->stat) {
        uint8_t kstat[128];
        int ret = ops->stat(relpath, kstat);
        if (ret == 0) {
            if (copyout(current_proc->pagetable, tf->a2, kstat, 128) < 0) {
                return (long)-14;
            }
        }
        return (long)(int64_t)ret;
    } else {
        return (long)-1;
    }
}

static long sys_getdents64_handler(struct trapframe *tf) {
    int fd             = (int)tf->a0;
    uint64_t ubuf_va   = tf->a1;
    unsigned int count = (unsigned int)tf->a2;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;

    if (!f || f->is_pipe) {
        return (long)-1;
    }

    if (f->ops && f->ops->readdir) {
        if (count > 4096) count = 4096;
        char *kbuf = kalloc();
        if (!kbuf) {
            return (long)-12;
        }
        memset(kbuf, 0, 4096);

        int ret = f->ops->readdir(f->private, kbuf, count, &f->offset);
        if (ret < 0) {
            kfree(kbuf);
            return (long)(int64_t)ret;
        } else {
            unsigned int written = (unsigned int)ret;
            if (ret == 0 && f->offset >= 0) {
                f->offset = -1024;
            }
            if (f->offset <= -1024) {
                const char *mnames[8];
                int nmounts = vfs_get_child_mounts(f->path, mnames, 8);
                int mstart = (int)(-(f->offset + 1024));
                for (int mi = mstart; mi < nmounts; mi++) {
                    int nl = strlen(mnames[mi]);
                    int rl = (19 + nl + 1 + 7) & ~7;
                    if (written + (unsigned int)rl > count) break;
                    char *d = kbuf + written;
                    memset(d, 0, rl);
                    uint64_t ino = (uint64_t)(5000 + mi);
                    int64_t doff = (int64_t)(-(1024 + mi + 1));
                    uint16_t rlen = (uint16_t)rl;
                    uint8_t dt = 4;
                    memcpy(d + 0, &ino, 8);
                    memcpy(d + 8, &doff, 8);
                    memcpy(d + 16, &rlen, 2);
                    memcpy(d + 18, &dt, 1);
                    memcpy(d + 19, mnames[mi], nl + 1);
                    written += (unsigned int)rl;
                    f->offset = (int64_t)(-(1024 + mi + 1));
                }
            }
            if (written > 0) {
                if (copyout(current_proc->pagetable, ubuf_va, kbuf, written) < 0) {
                    kfree(kbuf);
                    return (long)-14;
                }
            }
            kfree(kbuf);
            return (long)written;
        }
    } else {
        return (long)-1;
    }
}

static long sys_nanosleep_handler(struct trapframe *tf) {
    uint64_t req_uaddr = tf->a0;
    if (req_uaddr && current_proc) {
        uint64_t kreq[2];
        if (copyin(current_proc->pagetable, kreq, req_uaddr, 16) < 0)
            return (long)-14;
        if (kreq[1] >= 1000000000UL)
            return (long)-22;
        if (kreq[0] != 0 || kreq[1] != 0) {
            current_proc->sleep_until = timespec_rel_deadline(kreq[0], kreq[1]);
            current_proc->waiting_for_uart = 2;
            current_proc->state = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
        }
    }
    return (long)0;
}

static long sys_exit_handler(struct trapframe *tf) {
    int status = (int)tf->a0;
    if (current_proc) {
        current_proc->exit_status = status;
        current_proc->killed_by_signal = 0;
        cleanup_proc_resources(current_proc);
        if (current_proc->pid == 1) {
            printk("\nSystem halted (init exited with status %d).\n", status);
            register unsigned long a0 asm("a0") = 0;
            register unsigned long a1 asm("a1") = 0;
            register unsigned long a7 asm("a7") = 0x53525354;
            asm volatile("ecall" : : "r"(a0), "r"(a1), "r"(a7));
            while (1) {}
        }
        current_proc->state = ZOMBIE;
        if (current_proc->parent) {
            current_proc->parent->sigpending |= signal_bit(SIGCHLD);
            if (current_proc->parent->state == SLEEPING)
                current_proc->parent->state = RUNNABLE;
        }
        for (struct proc *p = proc; p < &proc[64]; p++) {
            if (p->state == SLEEPING && p != current_proc &&
                p->waiting_for_uart == 0)
                p->state = RUNNABLE;
        }
        swtch(&current_proc->context, &scheduler_context);
    }
    while(1) {}
    return SYSCALL_NORETURN;
}

/* SYS_reboot — magic values from Linux ABI:
 *   LINUX_REBOOT_MAGIC1  = 0xfee1dead
 *   LINUX_REBOOT_MAGIC2  = 0x28121969
 *   LINUX_REBOOT_CMD_POWER_OFF = 0x4321fedc
 *   LINUX_REBOOT_CMD_RESTART   = 0x01234567
 * We accept the correct magic pair and then call SBI SRST extension.
 * If called without correct magic, return -EPERM. */
#define REBOOT_MAGIC1      0xfee1deadU
#define REBOOT_MAGIC2      0x28121969U
#define REBOOT_CMD_HALT    0xcdef0123U
#define REBOOT_CMD_POWEROFF 0x4321fedcU
#define REBOOT_CMD_RESTART 0x01234567U

static long sys_reboot_handler(struct trapframe *tf) {
    uint32_t magic1 = (uint32_t)tf->a0;
    uint32_t magic2 = (uint32_t)tf->a1;
    uint32_t cmd    = (uint32_t)tf->a2;

    if (magic1 != REBOOT_MAGIC1 || magic2 != REBOOT_MAGIC2)
        return (long)-1; /* -EPERM */

    if (cmd == REBOOT_CMD_RESTART) {
        printk("\nSystem rebooting...\n");
        /* SBI SRST extension (EID=0x53525354, FID=0): cold reboot (reset_type=1) */
        register unsigned long a0 asm("a0") = 1; /* reset_type: cold reboot */
        register unsigned long a1 asm("a1") = 0; /* reset_reason: none */
        register unsigned long a6 asm("a6") = 0; /* function ID */
        register unsigned long a7 asm("a7") = 0x53525354; /* EID: SRST */
        asm volatile("ecall" : : "r"(a0), "r"(a1), "r"(a6), "r"(a7));
    } else {
        /* POWEROFF or HALT */
        printk("\nSystem shutting down...\n");
        /* SBI SRST extension (EID=0x53525354, FID=0): shutdown (reset_type=0) */
        register unsigned long a0 asm("a0") = 0; /* reset_type: shutdown */
        register unsigned long a1 asm("a1") = 0; /* reset_reason: none */
        register unsigned long a6 asm("a6") = 0; /* function ID */
        register unsigned long a7 asm("a7") = 0x53525354; /* EID: SRST */
        asm volatile("ecall" : : "r"(a0), "r"(a1), "r"(a6), "r"(a7));
        /* Fallback: SBI v0.1 legacy shutdown (EID=8) */
        register unsigned long fa7 asm("a7") = 8;
        asm volatile("ecall" : : "r"(fa7));
    }
    while (1) {}
    return SYSCALL_NORETURN;
}

static long sys_mmap_handler(struct trapframe *tf) {
    uint64_t addr  = tf->a0;
    uint64_t len   = tf->a1;
    int      prot  = (int)tf->a2;
    int      flags = (int)tf->a3;

    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (len == 0) {
        return (long)-1;
    }

    uint64_t va;
    if (flags & MAP_FIXED) {
        va = addr & ~(uint64_t)(PAGE_SIZE - 1);
    } else {
        if (current_proc->mmap_base == 0)
            current_proc->mmap_base = 0x20000000UL;
        va = current_proc->mmap_base;
        current_proc->mmap_base += len;
    }

    int pte_flags = 0;
    if (prot & PROT_READ)  pte_flags |= PTE_R;
    if (prot & PROT_WRITE) pte_flags |= PTE_W;
    if (prot & PROT_EXEC)  pte_flags |= PTE_X;
    if (pte_flags == 0) pte_flags = PTE_R; /* minimum readable */

    if (uvmalloc(current_proc->pagetable, va, va + len, pte_flags) == 0) {
        return (long)-1;
    }

    if (vma_add(current_proc, va, va + len, prot, flags) < 0) {
        uvmunmap(current_proc->pagetable, va, len);
        return (long)-1;
    }

    return (long)va;
}

static long sys_munmap_handler(struct trapframe *tf) {
    uint64_t addr = tf->a0 & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t len  = tf->a1;
    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    if (addr >= MAXVA || addr + len > MAXVA)
        return -22; /* -EINVAL */

    /* Unmap pages -- uvmunmap handles unmapped gaps gracefully */
    uvmunmap(current_proc->pagetable, addr, len);
    sfence_vma();

    /* Remove VMA tracking (handles splits automatically) */
    vma_remove(current_proc, addr, addr + len);

    return 0;
}

static long sys_mprotect_handler(struct trapframe *tf) {
    uint64_t addr = tf->a0;
    uint64_t len  = tf->a1;
    int prot      = (int)tf->a2;

    /* Validate alignment */
    if (addr & (PAGE_SIZE - 1))
        return -22; /* -EINVAL */
    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (len == 0)
        return 0;
    if (addr + len > MAXVA)
        return -22;

    /* Build new PTE permission bits */
    int new_flags = 0;
    if (prot & PROT_READ)  new_flags |= PTE_R;
    if (prot & PROT_WRITE) new_flags |= PTE_W;
    if (prot & PROT_EXEC)  new_flags |= PTE_X;

    /* Walk each page and update PTE flags */
    for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE) {
        int level;
        pte_t *pte = walk_super(current_proc->pagetable, va, &level);

        /* Split super page if needed */
        if (pte && (*pte & PTE_V) && level == 1) {
            superpage_split(current_proc->pagetable, va & ~(uint64_t)SUPERPAGE_MASK);
            pte = walk(current_proc->pagetable, va, 0);
        }

        if (!pte || !(*pte & PTE_V))
            continue;

        /* Clear old R/W/X, set new ones, keep U and other flags */
        *pte = (*pte & ~(PTE_R | PTE_W | PTE_X)) | new_flags;
    }

    /* Update VMA tracking: remove old region, re-add with new prot */
    struct vm_area *v = vma_find(current_proc, addr);
    int old_flags = v ? v->flags : 0;
    vma_remove(current_proc, addr, addr + len);
    vma_add(current_proc, addr, addr + len, prot, old_flags);

    sfence_vma();
    return 0;
}

static long sys_ps_handler(struct trapframe *tf) {
    int      maxn     = (int)tf->a1;
    int      written  = 0;
    if (tf->a0 && maxn > 0) {
        uint8_t kps[64 * 24];
        for (struct proc *p = proc; p < &proc[64]; p++) {
            if (p->state == UNUSED) continue;
            if (written >= maxn) break;
            char st;
            switch (p->state) {
                case SLEEPING:  st = 'S'; break;
                case RUNNABLE:  st = 'R'; break;
                case RUNNING:   st = 'R'; break;
                case ZOMBIE:    st = 'Z'; break;
                default:        st = '?'; break;
            }
            uint8_t *e = kps + written * 24;
            int32_t pid_  = (int32_t)p->pid;
            int32_t ppid_ = (int32_t)(p->parent ? p->parent->pid : 0);
            memset(e, 0, 24);
            memcpy(e + 0, &pid_,  4);
            memcpy(e + 4, &ppid_, 4);
            e[8] = (uint8_t)st;
            int nl = strlen(p->name);
            if (nl > 15) nl = 15;
            memcpy(e + 9, p->name, nl);
            written++;
        }
        if (written > 0) {
            if (copyout(current_proc->pagetable, tf->a0, kps, (uint64_t)written * 24) < 0) {
                return (long)-14;
            }
        }
    }
    return (long)written;
}

static long sys_ioctl_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    unsigned long request = tf->a1;
    uint64_t argp = tf->a2;

    if (request == 0x5413) { /* TIOCGWINSZ */
        struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;
        int is_tty = 0;
        if (f && f->ops == &devfs_ops && f->private)
          is_tty = devfs_is_tty(f->private);
        if (is_tty) {
            if (argp) {
                struct { uint16_t row, col, xpix, ypix; } ws = {24, 80, 0, 0};
                if (copyout(current_proc->pagetable, argp, &ws, sizeof(ws)) < 0) {
                    return (long)-14;
                }
            }
            return (long)0;
        } else {
            return (long)-25;
        }
    } else if (request == 0x540F || request == 0x5410) {
        /* TIOCGPGRP / TIOCSPGRP — delegate to tty_ioctl */
        int kpgid = 0;
        if (request == 0x5410 && argp) {
            if (copyin(current_proc->pagetable, &kpgid, argp, sizeof(kpgid)) < 0)
                return (long)-14;
        }
        long result = (long)tty_ioctl(request, (void *)&kpgid);
        if (request == 0x540F && argp) {
            if (copyout(current_proc->pagetable, argp, &kpgid, sizeof(kpgid)) < 0)
                return (long)-14;
        }
        return result;
    } else if (request >= TCGETS && request <= TCSETSF) {
        if (fd >= 0 && fd <= 2) {
            char ktermios[128];
            memset(ktermios, 0, sizeof(ktermios));
            if (request != TCGETS && argp) {
                if (copyin(current_proc->pagetable, ktermios, argp, 60) < 0) {
                    return (long)-14;
                }
            }
            long result = (long)tty_ioctl(request, (void *)ktermios);
            if (request == TCGETS && argp) {
                if (copyout(current_proc->pagetable, argp, ktermios, 60) < 0) {
                    return (long)-14;
                }
            }
            return result;
        } else {
            return (long)-25;
        }
    } else {
        return (long)-25;
    }
}

static long sys_rt_sigaction_handler(struct trapframe *tf) {
    int signum = (int)tf->a0;
    uint64_t act_uaddr = tf->a1;
    uint64_t oldact_uaddr = tf->a2;

    if (signum < 1 || signum >= 32 || signum == 9 || signum == 19) {
        return (long)-22;
    } else {
        struct kern_sigaction *sig_sa = proc_sigactions(current_proc);
        if (oldact_uaddr) {
            struct {
                uint64_t handler; uint64_t mask;
                int flags; int pad; uint64_t restorer;
            } old;
            old.handler = sig_sa[signum].handler;
            old.mask = sig_sa[signum].mask;
            old.flags = sig_sa[signum].flags;
            old.pad = 0;
            old.restorer = sig_sa[signum].restorer;
            if (copyout(current_proc->pagetable, oldact_uaddr, &old, sizeof(old)) < 0) {
                return (long)-14;
            }
        }
        if (act_uaddr) {
            struct {
                uint64_t handler; uint64_t mask;
                int flags; int pad; uint64_t restorer;
            } act;
            if (copyin(current_proc->pagetable, &act, act_uaddr, sizeof(act)) < 0) {
                return (long)-14;
            }
            sig_sa[signum].handler = act.handler;
            sig_sa[signum].mask = act.mask;
            sig_sa[signum].flags = act.flags;
            sig_sa[signum].restorer = act.restorer;
        }
        return (long)0;
    }
}

static long sys_rt_sigprocmask_handler(struct trapframe *tf) {
    int how = (int)tf->a0;
    uint64_t set_uaddr = tf->a1;
    uint64_t oldset_uaddr = tf->a2;

    if (oldset_uaddr) {
        uint64_t oldmask = current_proc->sigmask;
        if (copyout(current_proc->pagetable, oldset_uaddr, &oldmask, sizeof(oldmask)) < 0) {
            return (long)-14;
        }
    }
    if (set_uaddr) {
        uint64_t newset = 0;
        if (copyin(current_proc->pagetable, &newset, set_uaddr, sizeof(newset)) < 0) {
            return (long)-14;
        }
        if (how == 0)
            current_proc->sigmask |= newset;
        else if (how == 1)
            current_proc->sigmask &= ~newset;
        else if (how == 2)
            current_proc->sigmask = newset;
    }
    return (long)0;
}

static long sys_rt_sigreturn_handler(struct trapframe *tf) {
    uint64_t usp = tf->sp;

    struct signal_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (copyin(current_proc->pagetable, &frame, usp, sizeof(frame)) < 0) {
        return (long)-14;
    }

    current_proc->sigmask = frame.saved_sigmask;
    memcpy(tf, &frame.saved_tf, sizeof(frame.saved_tf));
    return SYSCALL_NORETURN;
}

static long sys_gettimeofday_handler(struct trapframe *tf) {
    uint64_t tv_uaddr = tf->a0;
    if (tv_uaddr) {
        int64_t sec, nsec;
        get_wall_time(&sec, &nsec);
        struct { int64_t sec; int64_t usec; } ktv;
        ktv.sec = sec;
        ktv.usec = nsec / 1000;
        if (copyout(current_proc->pagetable, tv_uaddr, &ktv, sizeof(ktv)) < 0) {
            return (long)-14;
        }
    }
    return (long)0;
}

static long sys_fcntl_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    int cmd = (int)tf->a1;
    struct open_file **ftab = proc_ofile(current_proc);

    if (fd < 0 || fd >= NOFILE || !ftab[fd])
        return (long)-9; /* EBADF */

    if (cmd == 0 || cmd == 1030) {
        /* F_DUPFD (0) or F_DUPFD_CLOEXEC (1030) — dup to lowest fd >= arg */
        int minfd = (int)tf->a2;
        if (minfd < 0 || minfd >= NOFILE)
            return (long)-22; /* EINVAL */
        for (int i = minfd; i < NOFILE; i++) {
            if (ftab[i] == 0) {
                ftab[i] = ftab[fd];
                ftab[fd]->refcount++;
                if (ftab[fd]->is_pipe) {
                    int pid = ftab[fd]->pipe_id;
                    if (pid >= 0) {
                        if (ftab[fd]->pipe_read)  pipes[pid].read_open++;
                        if (ftab[fd]->pipe_write) pipes[pid].write_open++;
                    }
                }
                return (long)i;
            }
        }
        return (long)-24; /* EMFILE */
    } else if (cmd == 1) { /* F_GETFD */
        return (long)0;
    } else if (cmd == 2) { /* F_SETFD */
        return (long)0;
    } else if (cmd == 3) { /* F_GETFL */
        return (long)ftab[fd]->flags;
    } else if (cmd == 4) { /* F_SETFL */
        ftab[fd]->flags = (int)tf->a2;
        return (long)0;
    } else {
        return (long)-22; /* EINVAL */
    }
}

static long sys_faccessat_handler(struct trapframe *tf) {
    char kpath[256];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }

    char resolved[256];
    resolve_path(current_proc->cwd, kpath, resolved);

    const char *relpath;
    struct vfs_ops *ops = vfs_lookup_mount(resolved, &relpath);
    if (ops && ops->stat) {
        uint8_t st[128];
        int ret = ops->stat(relpath, st);
        return (ret == 0) ? (long)0 : (long)-2;
    } else {
        return (long)-2;
    }
}

static long sys_unlinkat_handler(struct trapframe *tf) {
    int flags = (int)tf->a2;
    char kpath[256];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }
    {
        char resolved[256];
        resolve_path(current_proc->cwd, kpath, resolved);
        const char *relpath;
        struct vfs_ops *ops = vfs_lookup_mount(resolved, &relpath);
        if (flags & 0x200) {
            if (ops && ops->rmdir) {
                int ret = ops->rmdir(relpath);
                return (long)(int64_t)ret;
            } else {
                return (long)-30;
            }
        } else {
            if (ops && ops->unlink) {
                int ret = ops->unlink(relpath);
                return (long)(int64_t)ret;
            } else {
                return (long)-30;
            }
        }
    }
}

static long sys_linkat_handler(struct trapframe *tf) {
    (void)tf;
    return (long)-30;
}

static long sys_renameat2_handler(struct trapframe *tf) {
    char kold[256], knew[256];
    if (copyinstr(current_proc->pagetable, kold, tf->a1, sizeof(kold)) < 0)
        return (long)-14;
    if (copyinstr(current_proc->pagetable, knew, tf->a3, sizeof(knew)) < 0)
        return (long)-14;

    char old_resolved[256], new_resolved[256];
    resolve_path(current_proc->cwd, kold, old_resolved);
    resolve_path(current_proc->cwd, knew, new_resolved);

    const char *old_relpath, *new_relpath;
    struct vfs_ops *old_ops = vfs_lookup_mount(old_resolved, &old_relpath);
    struct vfs_ops *new_ops = vfs_lookup_mount(new_resolved, &new_relpath);

    if (old_ops != new_ops)
        return (long)-18; /* EXDEV */
    if (!old_ops || !old_ops->rename)
        return (long)-30; /* EROFS */

    return (long)(int64_t)old_ops->rename(old_relpath, new_relpath);
}

static long sys_mkdirat_handler(struct trapframe *tf) {
    int mode = (int)tf->a2;
    char kpath[256];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }
    {
        char resolved[256];
        resolve_path(current_proc->cwd, kpath, resolved);
        const char *relpath;
        struct vfs_ops *ops = vfs_lookup_mount(resolved, &relpath);
        if (ops && ops->mkdir) {
            int ret = ops->mkdir(relpath, mode);
            return (long)(int64_t)ret;
        } else {
            return (long)-30;
        }
    }
}

static long sys_mknodat_handler(struct trapframe *tf) {
    uint32_t mode = (uint32_t)tf->a2;
    uint32_t rdev = (uint32_t)tf->a3;
    char kpath[256];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }
    {
        char resolved[256];
        resolve_path(current_proc->cwd, kpath, resolved);
        const char *relpath;
        struct vfs_ops *ops = vfs_lookup_mount(resolved, &relpath);
        if (ops == &ext2_ops) {
            int ret = ext2_mknod(relpath, mode, rdev);
            return (long)(int64_t)ret;
        } else {
            return (long)-30;
        }
    }
}

static long sys_umask_handler(struct trapframe *tf) {
    unsigned int new_mask = (unsigned int)tf->a0 & 0777;
    unsigned int old_mask = current_proc->umask_val;
    current_proc->umask_val = new_mask;
    return (long)old_mask;
}

static long sys_geteuid_handler(struct trapframe *tf) {
    (void)tf;
    return (long)0;
}

static long sys_getegid_handler(struct trapframe *tf) {
    (void)tf;
    return (long)0;
}

static long sys_set_tid_address_handler(struct trapframe *tf) {
    (void)tf;
    return (long)current_proc->pid;
}

static long sys_readlinkat_handler(struct trapframe *tf) {
    char kpath[256];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0)
        return (long)-14;

    int bufsiz = (int)tf->a3;
    if (bufsiz <= 0)
        return (long)-22;

    char resolved[256];
    resolve_path(current_proc->cwd, kpath, resolved);

    const char *relpath;
    struct vfs_ops *ops = vfs_lookup_mount(resolved, &relpath);
    if (!ops || !ops->readlink)
        return (long)-22; /* EINVAL */

    char kbuf[256];
    int maxread = bufsiz > 256 ? 256 : bufsiz;
    int ret = ops->readlink(relpath, kbuf, maxread);
    if (ret < 0)
        return (long)(int64_t)ret;

    if (copyout(current_proc->pagetable, tf->a2, kbuf, (uint64_t)ret) < 0)
        return (long)-14;
    return (long)ret;
}

static long sys_symlinkat_handler(struct trapframe *tf) {
    char ktarget[256], klinkpath[256];
    if (copyinstr(current_proc->pagetable, ktarget, tf->a0, sizeof(ktarget)) < 0)
        return (long)-14;
    if (copyinstr(current_proc->pagetable, klinkpath, tf->a2, sizeof(klinkpath)) < 0)
        return (long)-14;

    char resolved[256];
    resolve_path(current_proc->cwd, klinkpath, resolved);

    const char *relpath;
    struct vfs_ops *ops = vfs_lookup_mount(resolved, &relpath);
    if (ops && ops->symlink)
        return (long)(int64_t)ops->symlink(ktarget, relpath);
    return (long)-30; /* EROFS */
}

static long sys_writev_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint64_t iov_uaddr = tf->a1;
    int iovcnt = (int)tf->a2;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;
    if (!f || iovcnt < 0) {
        return (long)-9;
    } else {
        if (iovcnt > 16) iovcnt = 16;
        struct { uint64_t base; uint64_t len; } kiov[16];
        if (copyin(current_proc->pagetable, kiov, iov_uaddr, iovcnt * 16) < 0) {
            return (long)-14;
        }
        int64_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            uint64_t buf_uaddr = kiov[i].base;
            uint64_t len = kiov[i].len;
            if (len == 0) continue;
            if (!buf_uaddr) {
                if (total == 0) total = -14;
                break;
            }
            if (f->is_pipe) {
                int pid = f->pipe_id;
                if (pid < 0 || !f->pipe_write) { total = -9; break; }
                if (pipes[pid].read_open <= 0) {
                    current_proc->sigpending |= signal_bit(SIGPIPE);
                    if (total == 0) total = -32; /* -EPIPE */
                    goto writev_out;
                }
                char kchunk[512];
                uint64_t done = 0;
                while (done < len) {
                    uint64_t clen = len - done;
                    if (clen > sizeof(kchunk)) clen = sizeof(kchunk);
                    if (copyin(current_proc->pagetable, kchunk, buf_uaddr + done, clen) < 0) {
                        if (total == 0) total = -14;
                        goto writev_out;
                    }
                    for (uint64_t j = 0; j < clen; j++) {
                        while (pipe_avail(pid) >= PIPE_BUF) {
                            if (!pipes[pid].read_open) break;
                            current_proc->state = SLEEPING;
                            swtch(&current_proc->context, &scheduler_context);
                        }
                        if (!pipes[pid].read_open) {
                            current_proc->sigpending |= signal_bit(SIGPIPE);
                            if (total == 0) total = -32;
                            goto writev_out;
                        }
                        pipes[pid].buf[pipes[pid].wpos % PIPE_BUF] = kchunk[j];
                        pipes[pid].wpos++;
                    }
                    done += clen;
                }
                total += (int64_t)len;
            } else if (f->ops && f->ops->write) {
                /* O_APPEND: seek to end before each iov write */
                if ((f->flags & 02000) && f->ops->fstat) {
                    uint8_t st[128];
                    if (f->ops->fstat(f->private, st) == 0) {
                        int64_t fsize;
                        memcpy(&fsize, st + 48, 8);
                        f->offset = fsize;
                    }
                }
                char kchunk[512];
                uint64_t done = 0;
                while (done < len) {
                    uint64_t clen = len - done;
                    if (clen > sizeof(kchunk)) clen = sizeof(kchunk);
                    if (copyin(current_proc->pagetable, kchunk, buf_uaddr + done, clen) < 0) {
                        if (total == 0) total = -14;
                        goto writev_out;
                    }
                    int64_t n = f->ops->write(f->private, kchunk, clen, &f->offset);
                    if (n < 0) { if (total == 0) total = n; goto writev_out; }
                    total += n;
                    done += n;
                    if ((uint64_t)n < clen) break;
                }
            } else {
                if (total == 0) total = -9;
                break;
            }
        }
writev_out:
        return (long)total;
    }
}

static long sys_readv_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint64_t iov_uaddr = tf->a1;
    int iovcnt = (int)tf->a2;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;
    if (!f || iovcnt < 0) {
        return (long)-9;
    } else {
        if (iovcnt > 16) iovcnt = 16;
        struct { uint64_t base; uint64_t len; } kiov[16];
        if (copyin(current_proc->pagetable, kiov, iov_uaddr, iovcnt * 16) < 0) {
            return (long)-14;
        }
        int64_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            uint64_t buf_uaddr = kiov[i].base;
            uint64_t len = kiov[i].len;
            if (len == 0) continue;
            if (!buf_uaddr) {
                if (total == 0) total = -14;
                break;
            }
            if (f->is_pipe) {
                int pid = f->pipe_id;
                if (pid < 0 || !f->pipe_read) { total = -9; break; }
                while (pipe_avail(pid) == 0 && pipes[pid].write_open)  {
                    current_proc->state = SLEEPING;
                    swtch(&current_proc->context, &scheduler_context);
                }
                char kchunk[512];
                uint64_t j = 0;
                while (j < len && pipe_avail(pid) > 0) {
                    uint64_t clen = len - j;
                    if (clen > sizeof(kchunk)) clen = sizeof(kchunk);
                    uint64_t k;
                    for (k = 0; k < clen && pipe_avail(pid) > 0; k++) {
                        kchunk[k] = pipes[pid].buf[pipes[pid].rpos % PIPE_BUF];
                        pipes[pid].rpos++;
                    }
                    if (k > 0 && copyout(current_proc->pagetable, buf_uaddr + j, kchunk, k) < 0) {
                        if (total == 0) total = -14;
                        goto readv_out;
                    }
                    j += k;
                }
                total += (int64_t)j;
            } else if (f->ops && f->ops->read) {
                char kchunk[512];
                uint64_t done = 0;
                while (done < len) {
                    uint64_t clen = len - done;
                    if (clen > sizeof(kchunk)) clen = sizeof(kchunk);
                    int64_t n = f->ops->read(f->private, kchunk, clen, &f->offset);
                    if (n < 0) { if (total == 0) total = n; goto readv_out; }
                    if (n == 0) break;
                    if (copyout(current_proc->pagetable, buf_uaddr + done, kchunk, n) < 0) {
                        if (total == 0) total = -14;
                        goto readv_out;
                    }
                    total += n;
                    done += n;
                    if ((uint64_t)n < clen) break;
                }
            } else {
                if (total == 0) total = -9;
                break;
            }
        }
readv_out:
        return (long)total;
    }
}

static long sys_prlimit64_handler(struct trapframe *tf) {
    int resource = (int)tf->a1;
    uint64_t old_uaddr = tf->a3;
    if (old_uaddr) {
        uint64_t rlim[2];
        if (resource == 3) {
            rlim[0] = 8 * 1024 * 1024;
            rlim[1] = 0xFFFFFFFFFFFFFFFFULL;
        } else if (resource == 7) {
            rlim[0] = 16;
            rlim[1] = 16;
        } else {
            rlim[0] = 0xFFFFFFFFFFFFFFFFULL;
            rlim[1] = 0xFFFFFFFFFFFFFFFFULL;
        }
        if (copyout(current_proc->pagetable, old_uaddr, rlim, 16) < 0) {
            return (long)-14;
        }
    }
    return (long)0;
}

static long sys_getrandom_handler(struct trapframe *tf) {
    uint64_t uaddr = tf->a0;
    uint64_t buflen = tf->a1;
    if (uaddr && buflen > 0) {
        uint64_t state = r_time();
        uint64_t done = 0;
        while (done < buflen) {
            char krbuf[512];
            uint64_t chunk = buflen - done;
            if (chunk > sizeof(krbuf)) chunk = sizeof(krbuf);
            for (uint64_t i = 0; i < chunk; i++) {
                state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                krbuf[i] = (char)(state >> 33);
            }
            if (copyout(current_proc->pagetable, uaddr + done, krbuf, chunk) < 0)
                break;
            done += chunk;
        }
    }
    return (long)buflen;
}

static long sys_setpgid_handler(struct trapframe *tf) {
    pid_t target = (pid_t)tf->a0;
    pid_t pgid = (pid_t)tf->a1;
    if (target == 0) target = current_proc->pid;
    if (pgid == 0) pgid = target;
    long result = (long)-3;
    for (struct proc *p = proc; p < &proc[64]; p++) {
        if (p->pid == target && p->state != UNUSED) {
            p->pgid = pgid;
            result = 0;
            break;
        }
    }
    return result;
}

static long sys_setsid_handler(struct trapframe *tf) {
    (void)tf;
    current_proc->sid = current_proc->pid;
    current_proc->pgid = current_proc->pid;
    return (long)current_proc->pid;
}

static long sys_getpgid_handler(struct trapframe *tf) {
    pid_t target = (pid_t)tf->a0;
    if (target == 0) {
        return (long)current_proc->pgid;
    } else {
        long result = (long)-3;
        for (struct proc *p = proc; p < &proc[64]; p++) {
            if (p->pid == target && p->state != UNUSED) {
                result = (long)p->pgid;
                break;
            }
        }
        return result;
    }
}

static long sys_getsid_handler(struct trapframe *tf) {
    pid_t target = (pid_t)tf->a0;
    if (target == 0) {
        return (long)current_proc->sid;
    } else {
        long result = (long)-3;
        for (struct proc *p = proc; p < &proc[64]; p++) {
            if (p->pid == target && p->state != UNUSED) {
                result = (long)p->sid;
                break;
            }
        }
        return result;
    }
}

static long sys_alarm_handler(struct trapframe *tf) {
    unsigned long seconds = tf->a0;
    unsigned long remaining = 0;
    if (current_proc->alarm_ticks > 0 && current_proc->alarm_ticks > ticks)
        remaining = current_proc->alarm_ticks - ticks;
    if (seconds == 0)
        current_proc->alarm_ticks = 0;
    else
        current_proc->alarm_ticks = ticks + seconds;
    return (long)remaining;
}

static long sys_madvise_handler(struct trapframe *tf) {
    (void)tf;
    return (long)0;
}

static long sys_ftruncate_handler(struct trapframe *tf) {
    (void)tf;
    return (long)-30;
}

static long sys_tgkill_handler(struct trapframe *tf) {
    pid_t tid = (pid_t)tf->a1;
    int sig = (int)tf->a2;
    if (sig == 0) {
        return (long)0;
    } else if (signal_is_valid(sig)) {
        long result = (long)-3;
        for (struct proc *p = proc; p < &proc[64]; p++) {
            if (p->pid == tid && p->state != UNUSED) {
                p->sigpending |= signal_bit(sig);
                if (p->state == SLEEPING)
                    p->state = RUNNABLE;
                result = 0;
                break;
            }
        }
        return result;
    } else {
        return (long)-22;
    }
}

/* Poll event constants (must match libc/include/poll.h) */
#define KPOLLIN   0x0001
#define KPOLLOUT  0x0004
#define KPOLLERR  0x0008
#define KPOLLHUP  0x0010
#define KPOLLNVAL 0x0020

static short check_fd_ready(struct open_file *f, short events) {
    if (!f)
        return KPOLLNVAL;

    short revents = 0;

    if (f->is_pipe) {
        int pid = f->pipe_id;
        if (pid < 0 || !pipes[pid].used)
            return KPOLLNVAL;

        if (f->pipe_read) {
            if (events & KPOLLIN) {
                if (pipe_avail(pid) > 0)
                    revents |= KPOLLIN;
            }
            /* POLLHUP: write end closed and no data left */
            if (pipes[pid].write_open <= 0 && pipe_avail(pid) == 0)
                revents |= KPOLLHUP;
        }
        if (f->pipe_write) {
            if (events & KPOLLOUT) {
                if (pipe_avail(pid) < PIPE_BUF)
                    revents |= KPOLLOUT;
            }
            /* POLLERR: read end closed */
            if (pipes[pid].read_open <= 0)
                revents |= KPOLLERR;
        }
    } else {
        /* Regular file or device: always ready */
        revents = events & (KPOLLIN | KPOLLOUT);
    }

    return revents;
}

static long sys_clock_nanosleep_handler(struct trapframe *tf) {
    /* a0=clockid (ignored), a1=flags, a2=request, a3=remain */
    int flags = (int)tf->a1;
    uint64_t kreq[2]; /* {tv_sec, tv_nsec} */
    if (copyin(current_proc->pagetable, kreq, tf->a2, 16) < 0)
        return (long)-14;

    if (kreq[1] >= 1000000000UL)
        return (long)-22;

    uint64_t deadline = timespec_rel_deadline(kreq[0], kreq[1]);

    if (flags & 1) {
        /* TIMER_ABSTIME: kreq is absolute wall time */
        int64_t now_sec, now_nsec;
        get_wall_time(&now_sec, &now_nsec);
        uint64_t now_total = (uint64_t)now_sec * 1000000000UL + (uint64_t)now_nsec;
        uint64_t target = kreq[0] * 1000000000UL + kreq[1];
        if (target <= now_total)
            return 0; /* already past */
        uint64_t diff_ns = target - now_total;
        deadline = r_time() + ns_to_time_ticks(diff_ns);
    }

    current_proc->sleep_until = deadline;
    current_proc->waiting_for_uart = 2;
    current_proc->state = SLEEPING;
    swtch(&current_proc->context, &scheduler_context);

    /* If remain ptr and relative mode, report remaining */
    if (tf->a3 != 0 && !(flags & 1)) {
        uint64_t krem[2] = {0, 0};
        copyout(current_proc->pagetable, tf->a3, krem, 16);
    }
    return 0;
}

static long sys_ppoll_handler(struct trapframe *tf) {
    uint64_t fds_uaddr = tf->a0;
    int nfds = (int)tf->a1;
    uint64_t timeout_uaddr = tf->a2;
    uint64_t sigmask_uaddr = tf->a3;

    if (nfds < 0 || nfds > NOFILE)
        return (long)-22;

    /* Copyin pollfd array: each entry is {int fd; short events; short revents} = 8 bytes */
    uint8_t kfds[NOFILE * 8];
    int fds_size = nfds * 8;
    if (fds_size > 0 && copyin(current_proc->pagetable, kfds, fds_uaddr, (uint64_t)fds_size) < 0)
        return (long)-14;

    /* Handle sigmask swap */
    uint64_t old_mask = current_proc->sigmask;
    if (sigmask_uaddr != 0) {
        uint64_t new_mask;
        if (copyin(current_proc->pagetable, &new_mask, sigmask_uaddr, 8) < 0)
            return (long)-14;
        current_proc->sigmask = new_mask;
    }

    /* Parse timeout */
    int has_timeout = (timeout_uaddr != 0);
    uint64_t sleep_until_val = 0;
    if (has_timeout) {
        uint64_t kts[2];
        if (copyin(current_proc->pagetable, kts, timeout_uaddr, 16) < 0) {
            current_proc->sigmask = old_mask;
            return (long)-14;
        }
        if (kts[0] == 0 && kts[1] == 0) {
            /* Non-blocking poll */
        } else {
            if (kts[1] >= 1000000000UL) {
                current_proc->sigmask = old_mask;
                return (long)-22;
            }
            sleep_until_val = timespec_rel_deadline(kts[0], kts[1]);
        }
    }

    struct open_file **ftab = proc_ofile(current_proc);

    /* Check readiness */
    int ready = 0;
    for (int i = 0; i < nfds; i++) {
        int fd = *(int *)(kfds + i * 8);
        short events = *(short *)(kfds + i * 8 + 4);
        struct open_file *f = (fd >= 0 && fd < NOFILE) ? ftab[fd] : 0;
        short rev = check_fd_ready(f, events);
        *(short *)(kfds + i * 8 + 6) = rev;
        if (rev)
            ready++;
    }

    /* If none ready and timeout allows blocking */
    if (ready == 0 && (!has_timeout || sleep_until_val > r_time())) {
        if (sleep_until_val > 0) {
            current_proc->sleep_until = sleep_until_val;
            current_proc->waiting_for_uart = 2;
            current_proc->state = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
        }

        /* Re-check after wakeup */
        ready = 0;
        for (int i = 0; i < nfds; i++) {
            int fd = *(int *)(kfds + i * 8);
            short events = *(short *)(kfds + i * 8 + 4);
            struct open_file *f = (fd >= 0 && fd < NOFILE) ? ftab[fd] : 0;
            short rev = check_fd_ready(f, events);
            *(short *)(kfds + i * 8 + 6) = rev;
            if (rev)
                ready++;
        }
    }

    /* Restore sigmask */
    if (sigmask_uaddr != 0)
        current_proc->sigmask = old_mask;

    /* Copyout results */
    if (fds_size > 0 && copyout(current_proc->pagetable, fds_uaddr, kfds, (uint64_t)fds_size) < 0)
        return (long)-14;

    return (long)ready;
}

static long sys_pselect6_handler(struct trapframe *tf) {
    int nfds = (int)tf->a0;
    uint64_t readfds_uaddr = tf->a1;
    uint64_t writefds_uaddr = tf->a2;
    uint64_t exceptfds_uaddr = tf->a3;
    uint64_t timeout_uaddr = tf->a4;

    if (nfds < 0 || nfds > NOFILE)
        nfds = NOFILE;

    /* fd_set is 128 bytes but we only need first 8 bytes (64 fds) */
    uint64_t rfds = 0, wfds = 0, efds = 0;
    if (readfds_uaddr && copyin(current_proc->pagetable, &rfds, readfds_uaddr, 8) < 0)
        return (long)-14;
    if (writefds_uaddr && copyin(current_proc->pagetable, &wfds, writefds_uaddr, 8) < 0)
        return (long)-14;
    if (exceptfds_uaddr && copyin(current_proc->pagetable, &efds, exceptfds_uaddr, 8) < 0)
        return (long)-14;

    /* Parse timeout */
    int has_timeout = (timeout_uaddr != 0);
    uint64_t sleep_until_val = 0;
    if (has_timeout) {
        uint64_t kts[2];
        if (copyin(current_proc->pagetable, kts, timeout_uaddr, 16) < 0)
            return (long)-14;
        if (kts[0] == 0 && kts[1] == 0) {
            /* Non-blocking */
        } else {
            if (kts[1] >= 1000000000UL)
                return (long)-22;
            sleep_until_val = timespec_rel_deadline(kts[0], kts[1]);
        }
    }

    struct open_file **ftab = proc_ofile(current_proc);

    /* Check readiness */
    uint64_t out_rfds = 0, out_wfds = 0, out_efds = 0;
    int ready = 0;

    for (int fd = 0; fd < nfds; fd++) {
        uint64_t bit = 1UL << fd;
        struct open_file *f = (fd < NOFILE) ? ftab[fd] : 0;

        if (rfds & bit) {
            short rev = check_fd_ready(f, KPOLLIN);
            if (rev & (KPOLLIN | KPOLLHUP | KPOLLERR)) {
                out_rfds |= bit;
                ready++;
            }
        }
        if (wfds & bit) {
            short rev = check_fd_ready(f, KPOLLOUT);
            if (rev & KPOLLOUT) {
                out_wfds |= bit;
                ready++;
            }
        }
        if (efds & bit) {
            short rev = check_fd_ready(f, 0);
            if (rev & (KPOLLERR | KPOLLNVAL)) {
                out_efds |= bit;
                ready++;
            }
        }
    }

    /* Block if none ready and timeout allows */
    if (ready == 0 && (!has_timeout || sleep_until_val > r_time())) {
        if (sleep_until_val > 0) {
            current_proc->sleep_until = sleep_until_val;
            current_proc->waiting_for_uart = 2;
            current_proc->state = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
        }

        /* Re-check */
        out_rfds = out_wfds = out_efds = 0;
        ready = 0;
        for (int fd = 0; fd < nfds; fd++) {
            uint64_t bit = 1UL << fd;
            struct open_file *f = (fd < NOFILE) ? ftab[fd] : 0;
            if (rfds & bit) {
                short rev = check_fd_ready(f, KPOLLIN);
                if (rev & (KPOLLIN | KPOLLHUP | KPOLLERR)) { out_rfds |= bit; ready++; }
            }
            if (wfds & bit) {
                short rev = check_fd_ready(f, KPOLLOUT);
                if (rev & KPOLLOUT) { out_wfds |= bit; ready++; }
            }
            if (efds & bit) {
                short rev = check_fd_ready(f, 0);
                if (rev & (KPOLLERR | KPOLLNVAL)) { out_efds |= bit; ready++; }
            }
        }
    }

    /* Copyout modified fd_sets */
    if (readfds_uaddr && copyout(current_proc->pagetable, readfds_uaddr, &out_rfds, 8) < 0)
        return (long)-14;
    if (writefds_uaddr && copyout(current_proc->pagetable, writefds_uaddr, &out_wfds, 8) < 0)
        return (long)-14;
    if (exceptfds_uaddr && copyout(current_proc->pagetable, exceptfds_uaddr, &out_efds, 8) < 0)
        return (long)-14;

    return (long)ready;
}

static long sys_sigaltstack_handler(struct trapframe *tf) {
    uint64_t new_addr = tf->a0;
    uint64_t old_addr = tf->a1;

    /* stack_t layout: { uint64_t ss_sp; int32_t ss_flags; int32_t _pad; uint64_t ss_size; } = 24 bytes */
    if (old_addr != 0) {
        uint8_t kold[24];
        memset(kold, 0, sizeof(kold));
        *(uint64_t *)(kold + 0) = current_proc->ss_sp;
        *(int32_t *)(kold + 8) = (int32_t)current_proc->ss_flags;
        *(uint64_t *)(kold + 16) = current_proc->ss_size;
        if (copyout(current_proc->pagetable, old_addr, kold, 24) < 0)
            return (long)-14;
    }

    if (new_addr != 0) {
        /* Cannot change alt stack while executing on it */
        if (current_proc->ss_flags & 1) /* SS_ONSTACK */
            return (long)-1; /* EPERM */

        uint8_t knew[24];
        if (copyin(current_proc->pagetable, knew, new_addr, 24) < 0)
            return (long)-14;

        current_proc->ss_sp = *(uint64_t *)(knew + 0);
        current_proc->ss_flags = (int)(*(int32_t *)(knew + 8));
        current_proc->ss_size = *(uint64_t *)(knew + 16);
    }

    return 0;
}

/* Static storage for dynamically mounted paths */
static char mount_path_storage[8][256];
static int mount_path_next = 0;

static long sys_mount_handler(struct trapframe *tf) {
    char ksrc[256], ktgt[256], kfstype[64];
    if (copyinstr(current_proc->pagetable, ksrc, tf->a0, sizeof(ksrc)) < 0)
        return (long)-14;
    if (copyinstr(current_proc->pagetable, ktgt, tf->a1, sizeof(ktgt)) < 0)
        return (long)-14;
    if (copyinstr(current_proc->pagetable, kfstype, tf->a2, sizeof(kfstype)) < 0)
        return (long)-14;

    char resolved[256];
    resolve_path(current_proc->cwd, ktgt, resolved);

    /* Only ext2 supported */
    if (strcmp(kfstype, "ext2") != 0)
        return (long)-19; /* ENODEV */

    if (mount_path_next >= 8)
        return (long)-12; /* ENOMEM */

    /* Copy path to persistent storage (mount table stores pointers) */
    int rlen = strlen(resolved);
    memcpy(mount_path_storage[mount_path_next], resolved, (uint64_t)rlen + 1);
    vfs_register_mount(mount_path_storage[mount_path_next], &ext2_ops);
    mount_path_next++;
    return 0;
}

static long sys_umount2_handler(struct trapframe *tf) {
    char ktgt[256];
    if (copyinstr(current_proc->pagetable, ktgt, tf->a0, sizeof(ktgt)) < 0)
        return (long)-14;

    char resolved[256];
    resolve_path(current_proc->cwd, ktgt, resolved);
    return (long)vfs_unregister_mount(resolved);
}

static long sys_uname_handler(struct trapframe *tf) {
    /* struct utsname: 5 fields of 65 bytes each = 325 bytes */
    char kbuf[325];
    memset(kbuf, 0, sizeof(kbuf));

    /* sysname */
    memcpy(kbuf + 0, "SBUnix", 6);
    /* nodename */
    memcpy(kbuf + 65, "sbunix", 6);
    /* release */
    memcpy(kbuf + 130, "1.0.0", 5);
    /* version */
    memcpy(kbuf + 195, "SBUnix RISC-V64", 15);
    /* machine */
    memcpy(kbuf + 260, "riscv64", 7);

    if (copyout(current_proc->pagetable, tf->a0, kbuf, sizeof(kbuf)) < 0)
        return (long)-14; /* EFAULT */
    return 0;
}

/* ---- syscall_init: populate the dispatch table ---- */

void syscall_init(void) {
    syscall_table[SYS_brk]             = sys_brk_handler;
    syscall_table[SYS_write]           = sys_write_handler;
    syscall_table[SYS_read]            = sys_read_handler;
    syscall_table[SYS_openat]          = sys_openat_handler;
    syscall_table[SYS_close]           = sys_close_handler;
    syscall_table[SYS_dup3]            = sys_dup3_handler;
    syscall_table[SYS_pipe2]           = sys_pipe2_handler;
    syscall_table[SYS_getcwd]          = sys_getcwd_handler;
    syscall_table[SYS_chdir]           = sys_chdir_handler;
    syscall_table[SYS_clone]           = sys_clone_handler;
    syscall_table[SYS_execve]          = sys_execve_handler;
    syscall_table[SYS_wait4]           = sys_wait4_handler;
    syscall_table[SYS_getpid]          = sys_getpid_handler;
    syscall_table[SYS_getppid]         = sys_getppid_handler;
    syscall_table[SYS_getuid]          = sys_getuid_handler;
    syscall_table[SYS_getgid]          = sys_getgid_handler;
    syscall_table[SYS_kill]            = sys_kill_handler;
    syscall_table[SYS_clock_gettime]   = sys_clock_gettime_handler;
    syscall_table[SYS_lseek]           = sys_lseek_handler;
    syscall_table[SYS_fstat]           = sys_fstat_handler;
    syscall_table[SYS_newfstatat]      = sys_newfstatat_handler;
    syscall_table[SYS_getdents64]      = sys_getdents64_handler;
    syscall_table[SYS_nanosleep]       = sys_nanosleep_handler;
    syscall_table[SYS_exit]            = sys_exit_handler;
    syscall_table[SYS_exit_group]      = sys_exit_handler;
    syscall_table[SYS_mmap]            = sys_mmap_handler;
    syscall_table[SYS_munmap]          = sys_munmap_handler;
    syscall_table[SYS_mprotect]        = sys_mprotect_handler;
    syscall_table[SYS_ps]              = sys_ps_handler;
    syscall_table[SYS_ioctl]           = sys_ioctl_handler;
    syscall_table[SYS_rt_sigaction]    = sys_rt_sigaction_handler;
    syscall_table[SYS_rt_sigprocmask]  = sys_rt_sigprocmask_handler;
    syscall_table[SYS_rt_sigreturn]    = sys_rt_sigreturn_handler;
    syscall_table[SYS_gettimeofday]    = sys_gettimeofday_handler;
    syscall_table[SYS_fcntl]           = sys_fcntl_handler;
    syscall_table[SYS_faccessat]       = sys_faccessat_handler;
    syscall_table[SYS_unlinkat]        = sys_unlinkat_handler;
    syscall_table[SYS_linkat]          = sys_linkat_handler;
    syscall_table[SYS_renameat2]       = sys_renameat2_handler;
    syscall_table[SYS_mkdirat]         = sys_mkdirat_handler;
    syscall_table[SYS_mknodat]         = sys_mknodat_handler;
    syscall_table[SYS_umask]           = sys_umask_handler;
    syscall_table[SYS_geteuid]         = sys_geteuid_handler;
    syscall_table[SYS_getegid]         = sys_getegid_handler;
    syscall_table[SYS_set_tid_address] = sys_set_tid_address_handler;
    syscall_table[SYS_readlinkat]      = sys_readlinkat_handler;
    syscall_table[SYS_symlinkat]       = sys_symlinkat_handler;
    syscall_table[SYS_writev]          = sys_writev_handler;
    syscall_table[SYS_readv]           = sys_readv_handler;
    syscall_table[SYS_prlimit64]       = sys_prlimit64_handler;
    syscall_table[SYS_getrandom]       = sys_getrandom_handler;
    syscall_table[SYS_setpgid]         = sys_setpgid_handler;
    syscall_table[SYS_setsid]          = sys_setsid_handler;
    syscall_table[SYS_getpgid]         = sys_getpgid_handler;
    syscall_table[SYS_getsid]          = sys_getsid_handler;
    syscall_table[SYS_alarm]           = sys_alarm_handler;
    syscall_table[SYS_madvise]         = sys_madvise_handler;
    syscall_table[SYS_ftruncate]       = sys_ftruncate_handler;
    syscall_table[SYS_tgkill]          = sys_tgkill_handler;
    syscall_table[SYS_uname]           = sys_uname_handler;
    syscall_table[SYS_sigaltstack]     = sys_sigaltstack_handler;
    syscall_table[SYS_clock_nanosleep] = sys_clock_nanosleep_handler;
    syscall_table[SYS_ppoll]           = sys_ppoll_handler;
    syscall_table[SYS_pselect6]        = sys_pselect6_handler;
    syscall_table[SYS_mount]           = sys_mount_handler;
    syscall_table[SYS_umount2]         = sys_umount2_handler;
    syscall_table[SYS_reboot]          = sys_reboot_handler;
}

/* ------------------------------------------------------------------ */
/* Main trap handler                                                    */
/* ------------------------------------------------------------------ */

void trap_handler(struct trapframe *tf) {
    unsigned long cause = r_scause();
    unsigned long stval = r_stval();

    if (cause & SCAUSE_INTERRUPT) {
        if (cause == SCAUSE_S_TIMER_INT) {
            handle_timer_interrupt();
        } else if (cause == SCAUSE_S_EXTERNAL_INT) {
            handle_external_interrupt();
        } else {
            printk("Unknown interrupt: scause=0x%lx\n", cause);
        }
        /* Deliver pending signals before returning to user mode */
        if (current_proc && (current_proc->sigpending & ~current_proc->sigmask))
            deliver_signal(tf, current_proc);
        return;
    }

    /* Exception */
    if (cause != SCAUSE_ECALL_U) {
        if (cause == SCAUSE_LOAD_PAGE_FAULT ||
            cause == SCAUSE_STORE_PAGE_FAULT ||
            cause == SCAUSE_INST_PAGE_FAULT) {

            /* --- COW page fault (store fault with COW-marked PTE) --- */
            if (cause == SCAUSE_STORE_PAGE_FAULT && current_proc) {
                uint64_t va = PGROUNDDOWN(stval);
                pte_t *pte = walk(current_proc->pagetable, va, 0);

                if (pte && (*pte & PTE_V) && (*pte & PTE_COW)) {
                    uint64_t pa = PTE2PA(*pte);
                    int flags = PTE_FLAGS(*pte);

                    if (page_ref_get(pa) == 1) {
                        /* Only reference: just make it writable again */
                        *pte = PA2PTE(pa) | ((flags & ~PTE_COW) | PTE_W);
                    } else {
                        /* Multiple references: allocate new page and copy */
                        char *newpage = kalloc();
                        if (!newpage)
                            goto pagefault_kill;
                        memcpy(newpage, (void *)pa, PAGE_SIZE);
                        *pte = PA2PTE((uint64_t)newpage) | ((flags & ~PTE_COW) | PTE_W);
                        kfree((void *)pa); /* decrement refcount on old page */
                    }
                    /* Flush TLB for this VA */
                    asm volatile("sfence.vma %0, zero" : : "r"(va));
                    return; /* resume execution */
                }
            }

            /* --- Swap-in: PTE_V=0 with PTE_SWAP marker --- */
            if (current_proc) {
                uint64_t sva = PGROUNDDOWN(stval);
                pte_t *spte = walk(current_proc->pagetable, sva, 0);
                if (spte && PTE_IS_SWAPPED(*spte)) {
                    int slot = PTE_SWAP_SLOT(*spte);
                    int sflags = PTE_SWAP_FLAGS(*spte);

                    char *newpage = kalloc();
                    if (!newpage)
                        goto pagefault_kill;

                    swap_read(slot, newpage);
                    swap_free(slot);

                    /* Restore PTE with original permissions + PTE_V */
                    *spte = PA2PTE((uint64_t)newpage) | sflags | PTE_V;
                    asm volatile("sfence.vma %0, zero" : : "r"(sva));
                    return; /* resume faulting instruction */
                }
            }

            /* --- Stack auto-grow (any page fault near current stack) --- */
            if (current_proc && current_proc->stack_top > 0 &&
                stval >= current_proc->stack_bottom - 256 * PAGE_SIZE &&
                stval < current_proc->stack_top) {
                uint64_t va = PGROUNDDOWN(stval);
                int grew = 0;
                while (current_proc->stack_bottom > va) {
                    current_proc->stack_bottom -= PAGE_SIZE;
                    char *pg = kalloc();
                    if (!pg) break;
                    memset(pg, 0, PAGE_SIZE);
                    mappages(current_proc->pagetable, current_proc->stack_bottom,
                             PAGE_SIZE, (uint64_t)pg, PTE_R | PTE_W | PTE_U);
                    grew = 1;
                }
                if (grew && current_proc->stack_bottom <= va) {
                    asm volatile("sfence.vma %0, zero" : : "r"(va));
                    return; /* resume -- page is now mapped */
                }
            }

            /* --- Demand paging for VMA regions --- */
            if (current_proc) {
                struct vm_area *fvma = vma_find(current_proc, stval);
                if (fvma) {
                    uint64_t va = PGROUNDDOWN(stval);
                    /* Check if page is already mapped */
                    pte_t *pte = walk(current_proc->pagetable, va, 0);
                    if (!pte || !(*pte & PTE_V)) {
                        char *pg = kalloc();
                        if (pg) {
                            memset(pg, 0, PAGE_SIZE);
                            int perm = PTE_U;
                            if (fvma->prot & PROT_READ)  perm |= PTE_R;
                            if (fvma->prot & PROT_WRITE) perm |= PTE_W;
                            if (fvma->prot & PROT_EXEC)  perm |= PTE_X;
                            mappages(current_proc->pagetable, va, PAGE_SIZE,
                                     (uint64_t)pg, perm);
                            asm volatile("sfence.vma %0, zero" : : "r"(va));
                            return; /* resume */
                        }
                    }
                }
            }

pagefault_kill:
            printk("Page fault pid=%d epc=0x%lx stval=0x%lx cause=%lu\n",
                   current_proc ? current_proc->pid : -1, tf->epc, stval, cause);
            if (current_proc) {
                current_proc->sigpending |= signal_bit(SIGSEGV);
                deliver_signal(tf, current_proc);
                /* If deliver_signal returned (handler caught it), resume */
                return;
            }
            while(1) {}
        }
        printk("UNEXPECTED TRAP! scause: 0x%lx, epc: 0x%lx, tval: 0x%lx\n",
               cause, tf->epc, stval);
        while(1) {}
    }

    /* ---- ecall from user ---- */
    /* Advance past the ecall instruction */
    uint16_t *instr = (uint16_t *)tf->epc;
    int size = ((*instr & 0x3) == 0x3) ? 4 : 2;
    tf->epc += size;

    long sysnum = (long)tf->a7;

    if (sysnum >= 0 && sysnum < NSYSCALLS && syscall_table[sysnum]) {
        long ret = syscall_table[sysnum](tf);
        if (ret != SYSCALL_NORETURN)
            tf->a0 = (uint64_t)ret;
    } else {
        printk("Unknown syscall %ld from pid %d\n", sysnum, current_proc ? current_proc->pid : -1);
        tf->a0 = (uint64_t)-38; /* -ENOSYS */
    }

    /* Deliver pending signals before returning to user mode */
    if (current_proc && (current_proc->sigpending & ~current_proc->sigmask))
        deliver_signal(tf, current_proc);
    return;
}
