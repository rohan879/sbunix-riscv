#include <trap.h>
#include <printk.h>
#include <riscv.h>
#include <proc.h>
#include <vfs.h>
#include <syscall.h>
#include <uart.h>
#include <plic.h>
#include <kalloc.h>
#include <kmalloc.h>
#include <string.h>
#include <vm.h>
#include <exec.h>

#define TIMER_FREQ 10000000UL  // 10 MHz timebase

static unsigned long boot_time;
static unsigned long ticks;
static unsigned char last_key = 0;

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
  for (int i = 0; i < NOFILE; i++) {
    if (current_proc->ofile[i] == NULL)
      return i;
  }
  return -1;
}

/* Shallow copy fd table from src to dst (for clone), incrementing refcounts */
static void copy_ofile(struct proc *dst, struct proc *src) {
  for (int i = 0; i < NOFILE; i++) {
    dst->ofile[i] = src->ofile[i];
    if (src->ofile[i]) {
      src->ofile[i]->refcount++;
      /* bump pipe refcounts too */
      if (src->ofile[i]->is_pipe) {
        int pid = src->ofile[i]->pipe_id;
        if (src->ofile[i]->pipe_read && pid >= 0)
          pipes[pid].read_open++;
        if (src->ofile[i]->pipe_write && pid >= 0)
          pipes[pid].write_open++;
      }
    }
  }
}

/* Close an open_file: decrement refcount, handle pipe cleanup, free when zero.
 * Also clears the slot in the given proc's ofile[fd]. */
static void close_ofile(struct proc *p, int fd) {
  struct open_file *f = p->ofile[fd];
  if (!f) return;
  p->ofile[fd] = NULL;

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
    sbi_set_timer(r_time() + TIMER_FREQ);
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
            if (c == 0x03) {
                /* Ctrl-C: kill all non-init foreground processes */
                int killed_any = 0;
                for (struct proc *p = proc; p < &proc[64]; p++) {
                    if (p->state != UNUSED && p->pid > 1) {
                        p->exit_status = 130; /* 128 + SIGINT */
                        p->state = ZOMBIE;
                        killed_any = 1;
                        /* Close all fds to release pipe references */
                        for (int i = 0; i < NOFILE; i++) {
                            if (p->ofile[i])
                                close_ofile(p, i);
                        }
                    }
                }
                /* When children were killed the shell is blocked in waitpid.
                 * Print "^C\n" here for visual feedback, then wake all sleeping
                 * processes so waitpid returns.  Do NOT enqueue 0x03 — the shell
                 * will call read_line fresh after waitpid.
                 *
                 * When no children existed the shell is blocked in read() inside
                 * read_line.  Deliver 0x03 to read_line; read_line itself will
                 * print "^C\n" and restart the prompt (avoids a double-print). */
                if (killed_any) {
                    printk("^C\n");
                } else {
                    /* Shell was idle at prompt; deliver ^C to read_line */
                    uart_enqueue(0x03);
                }
            } else {
                uart_enqueue(c);
            }
            last_key = (unsigned char)c;
        }
        uart_wakeup();
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
            ticks >= p->sleep_until) {
            p->waiting_for_uart = 0;
            p->state = RUNNABLE;
        }
    }
}

/* ------------------------------------------------------------------ */
/* copy_pagetable_cow: COW-share user pages from src to dst           */
/* ------------------------------------------------------------------ */
static int copy_pagetable_cow(pagetable_t dst, pagetable_t src, uint64_t sz) {
    /* Walk through user virtual address space and COW-share PTE_U pages */
    for (uint64_t va = 0; va < sz; va += PAGE_SIZE) {
        pte_t *pte = walk(src, va, 0);
        if (!pte || !(*pte & PTE_V))
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
/* sys_clone: fork the current process                                  */
/* ------------------------------------------------------------------ */
static pid_t sys_clone(void) {
    struct proc *p = allocproc();
    if (!p) return -1;

    /* Copy cwd */
    memcpy(p->cwd, current_proc->cwd, sizeof(p->cwd));

    /* Copy name */
    memcpy(p->name, current_proc->name, sizeof(p->name));

    /* Copy file descriptors */
    copy_ofile(p, current_proc);

    /* Copy trapframe — child starts after the ecall just like parent */
    memcpy(p->trapframe, current_proc->trapframe, sizeof(struct trapframe));

    /* Child returns 0 from clone/fork */
    p->trapframe->a0 = 0;

    /* Build child page table: copy kernel/device identity maps first */
    pagetable_t new_pt = uvmcreate();
    if (!new_pt) goto bad;

    /* Identity map kernel RAM and I/O (same as exec) */
    if (mappages(new_pt, 0x80000000, PHYSTOP - 0x80000000, 0x80000000,
                 PTE_R | PTE_W | PTE_X) < 0) goto bad;
    if (mappages(new_pt, 0x10000000, PAGE_SIZE, 0x10000000, PTE_R | PTE_W) < 0) goto bad;
    if (mappages(new_pt, 0x2000000, 0x10000, 0x2000000, PTE_R | PTE_W) < 0) goto bad;
    if (mappages(new_pt, 0x0c000000, 0x400000, 0x0c000000, PTE_R | PTE_W) < 0) goto bad;

    /* COW-share user pages (code + data segments + stack) */
    if (copy_pagetable_cow(new_pt, current_proc->pagetable, MAXVA) < 0) goto bad;

    /* Map signal trampoline into child */
    uvmmap_sigtrampoline(new_pt);

    p->pagetable = new_pt;
    p->sz = current_proc->sz;
    p->parent = current_proc;

    /* Copy stack bounds */
    p->stack_top = current_proc->stack_top;
    p->stack_bottom = current_proc->stack_bottom;

    /* Copy mmap_base */
    p->mmap_base = current_proc->mmap_base;

    /* Deep copy VMA list from parent to child */
    vma_dup(current_proc, p);

    /* Child's kernel context: resume at forkret which will sret back to user */
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
                    /* encode exit status as Linux wait status */
                    *wstatus = (p->exit_status & 0xff) << 8;
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
        if (deliverable & (1UL << i)) {
            signum = i;
            break;
        }
    }

    /* Clear from pending */
    p->sigpending &= ~(1UL << signum);

    uint64_t handler = p->sigactions[signum].handler;

    /* SIG_DFL (0) -- default action */
    if (handler == 0) {
        /* Default: ignore for SIGCHLD, SIGURG, SIGWINCH */
        if (signum == SIGCHLD || signum == SIGURG || signum == SIGWINCH)
            return;
        /* Default: terminate process */
        p->exit_status = 128 + signum;
        /* Clean up resources (mirrors SYS_exit path) */
        vma_free_all(p);
        for (int i = 0; i < NOFILE; i++) {
            if (p->ofile[i])
                close_ofile(p, i);
        }
        p->state = ZOMBIE;
        /* Notify parent via SIGCHLD and wake it */
        if (p->parent) {
            p->parent->sigpending |= (1UL << SIGCHLD);
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

    /* Push original trapframe onto user stack */
    sp -= sizeof(struct trapframe);
    sp &= ~15UL; /* 16-byte align */

    /* Copy trapframe to user stack via page walk */
    for (uint64_t off = 0; off < sizeof(struct trapframe); off += 8) {
        pte_t *pte = walk(p->pagetable, sp + off, 0);
        if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
            uint64_t pa = PTE2PA(*pte) + ((sp + off) & 0xFFF);
            *(uint64_t *)pa = *(uint64_t *)((char *)tf + off);
        }
    }

    /* Set up trapframe to call handler(signum) with return to trampoline */
    tf->epc = handler;                   /* jump to signal handler */
    tf->a0 = (uint64_t)signum;           /* first arg: signal number */
    tf->sp = sp;                         /* stack points to saved trapframe */
    tf->ra = SIG_TRAMPOLINE_VA;          /* return address -> trampoline -> sigreturn */

    /* Block this signal during handler execution */
    p->sigmask |= p->sigactions[signum].mask;
    if (!(p->sigactions[signum].flags & 0x40000000)) /* SA_NODEFER */
        p->sigmask |= (1UL << signum);
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

            /* --- Stack auto-grow (any page fault near current stack) --- */
            if (current_proc && current_proc->stack_top > 0 &&
                stval >= current_proc->stack_bottom - 16 * PAGE_SIZE &&
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
            /* Kill the process */
            if (current_proc) {
                current_proc->exit_status = 139;  /* SIGSEGV */
                current_proc->state = ZOMBIE;
                /* Wake parent if waiting */
                if (current_proc->parent &&
                    current_proc->parent->state == SLEEPING)
                    current_proc->parent->state = RUNNABLE;
                swtch(&current_proc->context, &scheduler_context);
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

    /* ---- SYS_brk: heap / program-break management ---- */
    if (sysnum == SYS_brk) {
        uint64_t new_brk = tf->a0;
        uint64_t cur_sz  = current_proc->sz;

        if (new_brk == 0) {
            /* Query: return current break */
            tf->a0 = cur_sz;
        } else if (new_brk < cur_sz) {
            /* Shrink heap (ignore for simplicity; return current brk) */
            tf->a0 = cur_sz;
        } else {
            /* Grow heap: map new pages starting from the next page boundary
             * above the current break, so we never re-map already-mapped pages.
             * Heap pages must be writable (PTE_W). */
            uint64_t heap_start = (cur_sz + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t rounded    = (new_brk + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
            if (heap_start < rounded) {
                if (uvmalloc(current_proc->pagetable, heap_start, rounded, PTE_W) == 0) {
                    tf->a0 = cur_sz; /* allocation failed; return old brk */
                    goto syscall_done;
                }
            }
            current_proc->sz = new_brk;
            tf->a0 = new_brk;
        }

    } else if (sysnum == SYS_write) {
        int fd = (int)tf->a0;
        char *buf = (char *)tf->a1;
        size_t len = (size_t)tf->a2;
        struct open_file *f = (fd >= 0 && fd < NOFILE) ? current_proc->ofile[fd] : NULL;

        if ((fd == 1 || fd == 2) && (!f || !f->is_pipe)) {
            /* stdout / stderr (only when not redirected to a pipe) */
            size_t i;
            for (i = 0; i < len; i++)
                printk("%c", buf[i]);
            tf->a0 = (uint64_t)i;
        } else if (f && f->is_pipe && f->pipe_write) {
            int pid = f->pipe_id;
            size_t written = 0;
            while (written < len) {
                int avail = PIPE_BUF - pipe_avail(pid);
                if (avail <= 0) {
                    /* Yield and retry */
                    current_proc->state = SLEEPING;
                    swtch(&current_proc->context, &scheduler_context);
                    continue;
                }
                int n = len - written;
                if (n > avail) n = avail;
                for (int i = 0; i < n; i++) {
                    pipes[pid].buf[pipes[pid].wpos % PIPE_BUF] = buf[written + i];
                    pipes[pid].wpos++;
                }
                written += n;
                /* Wake any sleeping readers now that data is available (not timed sleeps) */
                for (struct proc *p = proc; p < &proc[64]; p++) {
                    if (p->state == SLEEPING && p != current_proc &&
                        p->waiting_for_uart != 2)
                        p->state = RUNNABLE;
                }
            }
            tf->a0 = written;
        } else if (f && f->ops && f->ops->write) {
            /* VFS write dispatch */
            int64_t ret = f->ops->write(f->private, buf, len, &f->offset);
            tf->a0 = (uint64_t)ret;
        } else {
            tf->a0 = (uint64_t)-1;
        }

    } else if (sysnum == SYS_read) {
        int fd = (int)tf->a0;
        char *buf = (char *)tf->a1;
        size_t len = (size_t)tf->a2;
        struct open_file *f = (fd >= 0 && fd < NOFILE) ? current_proc->ofile[fd] : NULL;

        if (fd == 0 && (!f || !f->is_pipe)) {
            /* stdin: UART raw byte delivery.
             * The kernel returns characters as-is; line editing (echo,
             * backspace, Ctrl-C handling) is done by the caller (sbush). */
            size_t i = 0;
            while (i < len) {
                char c;
                /* Wait until a character is available */
                while (uart_dequeue(&c) != 0) {
                    /* Poll hardware UART directly in case interrupt missed a char */
                    int hw_c = uart_getc();
                    if (hw_c != -1) {
                        uart_enqueue((char)hw_c);
                        continue;
                    }
                    /* Sleep until UART interrupt wakes us */
                    if (current_proc) {
                        current_proc->waiting_for_uart = 1;
                        current_proc->state = SLEEPING;
                        swtch(&current_proc->context, &scheduler_context);
                    }
                }
                if (c == '\r') c = '\n';
                buf[i++] = c;
                /* Return after each character so the caller can process it */
                break;
            }
            tf->a0 = (uint64_t)i;
        } else if (f && f->is_pipe && f->pipe_read) {
            int pid = f->pipe_id;
            size_t read_bytes = 0;
            while (read_bytes < len) {
                while (pipe_avail(pid) == 0) {
                    /* Check if write end is closed */
                    if (pipes[pid].write_open == 0) goto pipe_eof;
                    current_proc->state = SLEEPING;
                    swtch(&current_proc->context, &scheduler_context);
                }
                buf[read_bytes] = pipes[pid].buf[pipes[pid].rpos % PIPE_BUF];
                pipes[pid].rpos++;
                read_bytes++;
                /* Wake any writers that might be blocked (not timed sleeps) */
                for (struct proc *p = proc; p < &proc[64]; p++) {
                    if (p->state == SLEEPING && p != current_proc &&
                        p->waiting_for_uart != 2)
                        p->state = RUNNABLE;
                }
                break; /* return what's available */
            }
pipe_eof:
            tf->a0 = (uint64_t)read_bytes;
        } else if (f && f->ops && f->ops->read) {
            /* VFS read dispatch */
            int64_t ret = f->ops->read(f->private, buf, len, &f->offset);
            tf->a0 = (uint64_t)ret;
        } else {
            tf->a0 = (uint64_t)0; /* EOF for unknown fds */
        }

    } else if (sysnum == SYS_openat) {
        /* openat(AT_FDCWD, path, flags, mode) */
        const char *path = (const char *)tf->a1;
        int flags = (int)tf->a2;

        if (!path) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }

        /* Resolve path against cwd */
        char resolved[256];
        resolve_path(current_proc->cwd, path, resolved);

        /* Look up mount point via VFS */
        const char *relpath;
        struct vfs_ops *ops = vfs_lookup_mount(resolved, &relpath);
        if (!ops) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }

        /* Call filesystem open */
        void *priv = NULL;
        int ret = ops->open(relpath, flags, &priv);
        if (ret < 0) {
            tf->a0 = (uint64_t)ret;
        } else {
            /* Allocate open_file and fd slot */
            struct open_file *f = vfs_alloc_file();
            if (!f) {
                if (ops->close) ops->close(priv);
                tf->a0 = (uint64_t)-12; /* -ENOMEM */
                goto syscall_done;
            }
            int fd = alloc_fd();
            if (fd < 0) {
                if (ops->close) ops->close(priv);
                vfs_free_file(f);
                tf->a0 = (uint64_t)-1;
                goto syscall_done;
            }
            f->ops = ops;
            f->private = priv;
            f->offset = 0;
            f->flags = flags;
            f->is_pipe = 0;
            current_proc->ofile[fd] = f;
            tf->a0 = (uint64_t)fd;
        }

    } else if (sysnum == SYS_close) {
        int fd = (int)tf->a0;
        if (fd >= 0 && fd < NOFILE && current_proc->ofile[fd]) {
            close_ofile(current_proc, fd);
            tf->a0 = 0;
        } else {
            tf->a0 = (uint64_t)-1;
        }

    } else if (sysnum == SYS_dup3) {
        int oldfd = (int)tf->a0;
        int newfd = (int)tf->a1;
        if (oldfd < 0 || oldfd >= NOFILE || !current_proc->ofile[oldfd] ||
            newfd < 0 || newfd >= NOFILE) {
            tf->a0 = (uint64_t)-1;
        } else if (oldfd == newfd) {
            /* dup2(x, x) is a no-op that returns newfd */
            tf->a0 = (uint64_t)newfd;
        } else {
            /* Close newfd if open */
            if (current_proc->ofile[newfd])
                close_ofile(current_proc, newfd);
            /* Share the same open_file, bump refcount */
            current_proc->ofile[newfd] = current_proc->ofile[oldfd];
            current_proc->ofile[oldfd]->refcount++;
            /* bump pipe refcount for the new reference */
            if (current_proc->ofile[oldfd]->is_pipe) {
                int pid = current_proc->ofile[oldfd]->pipe_id;
                if (pid >= 0) {
                    if (current_proc->ofile[oldfd]->pipe_read)  pipes[pid].read_open++;
                    if (current_proc->ofile[oldfd]->pipe_write) pipes[pid].write_open++;
                }
            }
            tf->a0 = (uint64_t)newfd;
        }

    } else if (sysnum == SYS_pipe2) {
        int *fds = (int *)tf->a0;
        int pipeid = alloc_pipe();
        if (pipeid < 0) {
            tf->a0 = (uint64_t)-1;
        } else {
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
                current_proc->ofile[rfd] = rf;
                if (wf) wfd = alloc_fd();
            }
            if (wfd < 0 || !wf) {
                if (rfd >= 0) {
                    current_proc->ofile[rfd] = NULL;
                    vfs_free_file(rf);
                } else if (rf) {
                    vfs_free_file(rf);
                }
                if (wf) vfs_free_file(wf);
                pipes[pipeid].used = 0;
                tf->a0 = (uint64_t)-1;
            } else {
                wf->is_pipe = 1;
                wf->pipe_read = 0;
                wf->pipe_write = 1;
                wf->pipe_id = pipeid;
                wf->ops = NULL;
                current_proc->ofile[wfd] = wf;
                fds[0] = rfd;
                fds[1] = wfd;
                tf->a0 = 0;
            }
        }

    } else if (sysnum == SYS_getcwd) {
        char *buf = (char *)tf->a0;
        size_t sz = (size_t)tf->a1;
        const char *cwd = current_proc->cwd;
        if (!cwd[0]) cwd = "/";
        int len = strlen(cwd);
        if (buf && (size_t)(len + 1) <= sz) {
            memcpy(buf, cwd, len + 1);
            tf->a0 = (uint64_t)buf;
        } else {
            tf->a0 = (uint64_t)0;
        }

    } else if (sysnum == SYS_chdir) {
        const char *path = (const char *)tf->a0;
        if (!path) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }
        char resolved[256];
        resolve_path(current_proc->cwd, path, resolved);
        if (vfs_is_dir(resolved)) {
            memcpy(current_proc->cwd, resolved, sizeof(current_proc->cwd));
            tf->a0 = 0;
        } else {
            tf->a0 = (uint64_t)-1;
        }

    } else if (sysnum == SYS_clone) {
        /* fork() is implemented as clone(SIGCHLD, 0, 0, 0, 0) */
        pid_t child = sys_clone();
        tf->a0 = (uint64_t)child;

    } else if (sysnum == SYS_execve) {
        const char *path = (const char *)tf->a0;
        char **argv = (char **)tf->a1;
        char **envp = (char **)tf->a2;

        if (!path) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }

        /* ---- Copy argv and envp strings into kernel-heap buffers BEFORE exec()
         * argv/envp pointers live in the old user address space.  After exec()
         * replaces the page table those VA->PA mappings no longer exist.
         * We allocate 4K pages for string data to avoid kernel stack overflow. */
        #define EXECVE_MAXARGS  32
        #define EXECVE_MAXENVS  32

        /* Use kalloc() for the large string buffer to avoid stack overflow */
        char *kstrbuf = (char *)kalloc();
        if (!kstrbuf) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }

        int kargc = 0;
        int kstroff = 0;
        int kstroff_arr[EXECVE_MAXARGS];

        if (argv) {
            while (kargc < EXECVE_MAXARGS && argv[kargc]) {
                const char *s = argv[kargc];
                int slen = strlen(s) + 1;
                if (kstroff + slen > (int)PAGE_SIZE) break;
                kstroff_arr[kargc] = kstroff;
                memcpy(kstrbuf + kstroff, s, slen);
                kstroff += slen;
                kargc++;
            }
        }

        /* Copy envp strings from user space (before exec destroys old page table) */
        char *kenvbuf = (char *)kalloc();
        int kenvc = 0;
        int kenvoff = 0;
        int kenvoff_arr[EXECVE_MAXENVS];

        if (kenvbuf && envp) {
            while (kenvc < EXECVE_MAXENVS && envp[kenvc]) {
                const char *s = envp[kenvc];
                int slen = strlen(s) + 1;
                if (kenvoff + slen > (int)PAGE_SIZE) break;
                kenvoff_arr[kenvc] = kenvoff;
                memcpy(kenvbuf + kenvoff, s, slen);
                kenvoff += slen;
                kenvc++;
            }
        }

        /* Also copy the path before exec() changes the address space */
        char kpath[256];
        {
            int plen = strlen(path);
            if (plen > 255) plen = 255;
            memcpy(kpath, path, plen + 1);
        }

        /* Resolve path relative to cwd, then convert to tarfs format */
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
        if (ret < 0) {
            kfree(kstrbuf);
            if (kenvbuf) kfree(kenvbuf);
            tf->a0 = (uint64_t)-1;
            return;
        }

        /* ---- Set up the new user stack with argc/argv/envp ---- */
        /* Stack layout at sp (growing down from MAXVA-32):
         *   sp+0:                              argc (8 bytes)
         *   sp+8:                              argv[0..argc-1] + NULL ((argc+1)*8)
         *   sp+8+(argc+1)*8:                   envp[0..envc-1] + NULL ((envc+1)*8)
         *   sp+8+(argc+1)*8+(envc+1)*8:        argv string data (kstroff bytes)
         *   sp+8+(argc+1)*8+(envc+1)*8+kstroff: envp string data (kenvoff bytes)
         */
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

            /* Use kalloc for kbuf too to avoid stack overflow */
            char *kbuf = (char *)kalloc();
            if (kbuf) {
                int off = 0;
                /* argc */
                uint64_t u_argc = (uint64_t)kargc;
                memcpy(kbuf + off, &u_argc, 8); off += 8;

                /* argv pointers */
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

                /* envp pointers */
                str_off2 = 0;
                for (int i = 0; i < kenvc; i++) {
                    ptrs[i] = envp_str_base + str_off2;
                    int slen = strlen(kenvbuf + kenvoff_arr[i]) + 1;
                    str_off2 += slen;
                }
                ptrs[kenvc] = 0;
                memcpy(kbuf + off, ptrs, envp_ptrs_sz);
                off += envp_ptrs_sz;

                /* argv string data */
                if (kstroff > 0) {
                    memcpy(kbuf + off, kstrbuf, kstroff);
                    off += kstroff;
                }

                /* envp string data */
                if (kenvoff > 0) {
                    memcpy(kbuf + off, kenvbuf, kenvoff);
                    off += kenvoff;
                }

                uvmcopyin(current_proc->pagetable, sp, kbuf, off);
                kfree(kbuf);
            }

            newtf->sp = sp;
            newtf->a0 = (uint64_t)kargc;
            newtf->a1 = argv_ptr_base;
            newtf->a2 = envp_ptr_base;
        }

        kfree(kstrbuf);
        if (kenvbuf) kfree(kenvbuf);

        /* exec updated the trapframe; trap_restore will sret into new process */
        trap_restore(current_proc->trapframe);
        /* never reached */

    } else if (sysnum == SYS_wait4) {
        pid_t wpid   = (pid_t)(int)tf->a0;
        int *wstatus = (int *)tf->a1;
        int options  = (int)tf->a2;
        tf->a0 = (uint64_t)sys_wait4(wpid, wstatus, options);

    } else if (sysnum == SYS_getpid) {
        tf->a0 = current_proc ? (uint64_t)current_proc->pid : 0;

    } else if (sysnum == SYS_getppid) {
        if (current_proc && current_proc->parent)
            tf->a0 = (uint64_t)current_proc->parent->pid;
        else
            tf->a0 = 1; /* init is own parent by convention */

    } else if (sysnum == SYS_getuid) {
        tf->a0 = 0; /* always root */

    } else if (sysnum == SYS_getgid) {
        tf->a0 = 0; /* always root */

    } else if (sysnum == SYS_kill) {
        pid_t target_pid = (pid_t)(int)tf->a0;
        int sig = (int)tf->a1;
        if (target_pid <= 0) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }
        struct proc *target = NULL;
        for (int i = 0; i < 64; i++) {
            if (proc[i].state != UNUSED && proc[i].pid == target_pid) {
                target = &proc[i];
                break;
            }
        }
        if (!target) {
            tf->a0 = (uint64_t)-3; /* -ESRCH */
        } else {
            if (sig == 9) { /* SIGKILL -- always kill immediately */
                target->exit_status = 128 + 9;
                target->state = ZOMBIE;
                if (target->parent && target->parent->state == SLEEPING)
                    target->parent->state = RUNNABLE;
            } else if (sig > 0 && sig < 32) {
                target->sigpending |= (1UL << sig);
                /* Wake target if sleeping so it can handle the signal */
                if (target->state == SLEEPING)
                    target->state = RUNNABLE;
            }
            tf->a0 = 0;
        }

    } else if (sysnum == SYS_clock_gettime) {
        /* clock_gettime(clockid_t, struct timespec *)
         * struct timespec: { int64_t tv_sec; int64_t tv_nsec; }
         * We report ticks as seconds (each tick = 1s). */
        int64_t *ts = (int64_t *)tf->a1;
        if (ts) {
            ts[0] = (int64_t)ticks;   /* tv_sec */
            ts[1] = 0;                 /* tv_nsec */
            tf->a0 = 0;
        } else {
            tf->a0 = (uint64_t)-1;
        }

    } else if (sysnum == SYS_lseek) {
        int fd      = (int)tf->a0;
        long offset = (long)tf->a1;
        int whence  = (int)tf->a2;
        struct open_file *f = (fd >= 0 && fd < NOFILE) ? current_proc->ofile[fd] : NULL;
        if (!f || f->is_pipe) {
            tf->a0 = (uint64_t)-1;
        } else {
            /* For VFS files, use the offset field directly.
             * We don't know the total file size here, so just update offset. */
            int64_t cur = f->offset;
            int64_t new_off;
            if (whence == 0)       /* SEEK_SET */ new_off = (int64_t)offset;
            else if (whence == 1)  /* SEEK_CUR */ new_off = cur + (int64_t)offset;
            else if (whence == 2)  /* SEEK_END */ {
                /* Need file size: try stat if available */
                if (f->ops && f->ops->stat) {
                    /* We don't easily have the path. For tarfs, offset is clamped
                     * during read anyway. Just set to offset (treat as 0+offset for end). */
                    new_off = (int64_t)offset; /* SEEK_END without size info is limited */
                } else {
                    new_off = (int64_t)offset;
                }
            }
            else { tf->a0 = (uint64_t)-1; goto syscall_done; }
            if (new_off < 0) new_off = 0;
            f->offset = new_off;
            tf->a0 = (uint64_t)new_off;
        }

    } else if (sysnum == SYS_fstat) {
        /* fstat(int fd, struct stat *st) */
        int fd = (int)tf->a0;
        uint8_t *st = (uint8_t *)tf->a1;
        struct open_file *f = (fd >= 0 && fd < NOFILE) ? current_proc->ofile[fd] : NULL;
        if (!st || !f) {
            tf->a0 = (uint64_t)-1;
        } else if (f->is_pipe) {
            /* Pipes: return FIFO mode */
            memset(st, 0, 128);
            uint32_t mode = 0x1000 | 0600; /* S_IFIFO | 0600 */
            memcpy(st + 16, &mode, 4);
            tf->a0 = 0;
        } else if (fd <= 2 && (!f || !f->ops)) {
            /* stdin/stdout/stderr without VFS backing (UART) */
            memset(st, 0, 128);
            uint32_t mode = 0x2000 | 0600; /* S_IFCHR | 0600 */
            memcpy(st + 16, &mode, 4);
            tf->a0 = 0;
        } else {
            /* For VFS files we don't easily have the path for stat.
             * Fill in a generic regular file stat from the open_file info. */
            memset(st, 0, 128);
            uint32_t mode = 0x81A4; /* S_IFREG | 0644 */
            uint32_t nlink = 1;
            int32_t blksz = 512;
            memcpy(st + 16, &mode, 4);
            memcpy(st + 20, &nlink, 4);
            memcpy(st + 56, &blksz, 4);
            tf->a0 = 0;
        }

    } else if (sysnum == SYS_newfstatat) {
        /* newfstatat(AT_FDCWD, path, struct stat *, flags) */
        const char *path = (const char *)tf->a1;
        uint8_t *st = (uint8_t *)tf->a2;
        if (!path || !st) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }
        char resolved[256];
        resolve_path(current_proc->cwd, path, resolved);

        /* Dispatch through VFS */
        const char *relpath;
        struct vfs_ops *ops = vfs_lookup_mount(resolved, &relpath);
        if (ops && ops->stat) {
            int ret = ops->stat(relpath, st);
            tf->a0 = (uint64_t)(int64_t)ret;
        } else {
            tf->a0 = (uint64_t)-1;
        }

    } else if (sysnum == SYS_getdents64) {
        /* getdents64(int fd, struct linux_dirent64 *buf, unsigned int count) */
        int fd             = (int)tf->a0;
        char *ubuf         = (char *)tf->a1;
        unsigned int count = (unsigned int)tf->a2;
        struct open_file *f = (fd >= 0 && fd < NOFILE) ? current_proc->ofile[fd] : NULL;

        if (!f || f->is_pipe) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }

        if (f->ops && f->ops->readdir) {
            int ret = f->ops->readdir(f->private, ubuf, count, &f->offset);
            tf->a0 = (uint64_t)(int64_t)ret;
        } else {
            tf->a0 = (uint64_t)-1;
        }

    } else if (sysnum == SYS_nanosleep) {
        /* nanosleep(const struct timespec *req, struct timespec *rem)
         * struct timespec: { uint64_t tv_sec; uint64_t tv_nsec; }
         * We sleep for tv_sec seconds (ignoring tv_nsec for simplicity).
         * One timer tick = 1 second (TIMER_FREQ = 10 MHz). */
        uint64_t *req = (uint64_t *)tf->a0;
        if (req && current_proc) {
            uint64_t secs = req[0]; /* tv_sec */
            current_proc->sleep_until     = ticks + secs;
            current_proc->waiting_for_uart = 2;
            current_proc->state           = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
        }
        tf->a0 = 0;

    } else if (sysnum == SYS_exit || sysnum == SYS_exit_group) {
        int status = (int)tf->a0;
        if (current_proc) {
            current_proc->exit_status = status;
            /* Free VMA list */
            vma_free_all(current_proc);
            /* Close all open fds */
            for (int i = 0; i < NOFILE; i++) {
                if (current_proc->ofile[i])
                    close_ofile(current_proc, i);
            }
            /* If init (pid 1) exits, the system has nothing left to run */
            if (current_proc->pid == 1) {
                printk("\nSystem halted (init exited with status %d).\n", status);
                /* Use SBI shutdown (extension 0x53525354 "SRST", function 0) */
                register unsigned long a0 asm("a0") = 0; /* shutdown */
                register unsigned long a1 asm("a1") = 0;
                register unsigned long a7 asm("a7") = 0x53525354;
                asm volatile("ecall" : : "r"(a0), "r"(a1), "r"(a7));
                while (1) {}
            }
            current_proc->state = ZOMBIE;
            /* Wake parent if it's waiting */
            if (current_proc->parent &&
                current_proc->parent->state == SLEEPING) {
                current_proc->parent->state = RUNNABLE;
            }
            /* Also wake any process waiting in wait4 (waiting_for_uart==0).
             * Do NOT wake timed-sleep processes (waiting_for_uart==2). */
            for (struct proc *p = proc; p < &proc[64]; p++) {
                if (p->state == SLEEPING && p != current_proc &&
                    p->waiting_for_uart == 0)
                    p->state = RUNNABLE;
            }
            swtch(&current_proc->context, &scheduler_context);
        }
        while(1) {}

    } else if (sysnum == SYS_mmap) {
        /* mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
         * Only MAP_ANONYMOUS supported.  Returns VA on success, (uint64_t)-1 on failure. */
        uint64_t addr  = tf->a0;
        uint64_t len   = tf->a1;
        int      prot  = (int)tf->a2;
        int      flags = (int)tf->a3;
        /* int fd = (int)tf->a4;  -- must be -1 for MAP_ANONYMOUS */
        /* off_t off = tf->a5;    -- must be 0 for MAP_ANONYMOUS */

        /* Round len up to page boundary */
        len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        if (len == 0) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }

        /* Choose virtual address */
        uint64_t va;
        if (flags & MAP_FIXED) {
            va = addr & ~(uint64_t)(PAGE_SIZE - 1);
        } else {
            /* Lazily initialise mmap_base to a safe region above the heap */
            if (current_proc->mmap_base == 0)
                current_proc->mmap_base = 0x20000000UL; /* 512 MB */
            va = current_proc->mmap_base;
            current_proc->mmap_base += len;
        }

        /* Map zeroed pages */
        if (uvmalloc(current_proc->pagetable, va, va + len, PTE_W) == 0) {
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }

        /* Track with VMA */
        if (vma_add(current_proc, va, va + len, prot, flags) < 0) {
            uvmunmap(current_proc->pagetable, va, len);
            tf->a0 = (uint64_t)-1;
            goto syscall_done;
        }

        tf->a0 = va;

    } else if (sysnum == SYS_munmap) {
        /* munmap(void *addr, size_t len) */
        uint64_t addr = tf->a0;
        uint64_t len  = tf->a1;

        len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

        /* Check VMA covers this region */
        struct vm_area *v = vma_find(current_proc, addr);
        if (v) {
            uvmunmap(current_proc->pagetable, addr, len);
            sfence_vma();
            vma_remove(current_proc, addr, addr + len);
            tf->a0 = 0;
        } else {
            tf->a0 = (uint64_t)-1;
        }

    } else if (sysnum == SYS_mprotect) {
        /* mprotect stub — always succeed */
        tf->a0 = 0;

    } else if (sysnum == SYS_ps) {
        /* ps(struct ps_entry *buf, int maxentries) -> number of entries written
         * struct ps_entry { int32_t pid; int32_t ppid; char state; char name[15]; }
         * = 20 bytes, padded to 24 for alignment. */
        uint8_t *buf      = (uint8_t *)tf->a0;
        int      maxn     = (int)tf->a1;
        int      written  = 0;
        if (buf && maxn > 0) {
            for (struct proc *p = proc; p < &proc[64]; p++) {
                if (p->state == UNUSED) continue;
                if (written >= maxn) break;
                /* encode state as single letter */
                char st;
                switch (p->state) {
                    case SLEEPING:  st = 'S'; break;
                    case RUNNABLE:  st = 'R'; break;
                    case RUNNING:   st = 'R'; break;
                    case ZOMBIE:    st = 'Z'; break;
                    default:        st = '?'; break;
                }
                uint8_t *e = buf + written * 24;
                int32_t pid_  = (int32_t)p->pid;
                int32_t ppid_ = (int32_t)(p->parent ? p->parent->pid : 0);
                memset(e, 0, 24);
                memcpy(e + 0, &pid_,  4);
                memcpy(e + 4, &ppid_, 4);
                e[8] = (uint8_t)st;
                /* copy name, max 15 chars */
                int nl = strlen(p->name);
                if (nl > 15) nl = 15;
                memcpy(e + 9, p->name, nl);
                written++;
            }
        }
        tf->a0 = (uint64_t)written;

    } else if (sysnum == SYS_ioctl) {
        int fd = (int)tf->a0;
        unsigned long request = tf->a1;
        uint64_t argp = tf->a2;

        if (request == 0x5413) { /* TIOCGWINSZ */
            if (fd >= 0 && fd <= 2) {
                /* stdin/stdout/stderr are the UART terminal */
                if (argp) {
                    /* Write a default 80x24 winsize struct to user memory */
                    struct { uint16_t row, col, xpix, ypix; } ws = {24, 80, 0, 0};
                    pte_t *pte = walk(current_proc->pagetable, argp, 0);
                    if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
                        uint64_t pa = PTE2PA(*pte) + (argp & 0xFFF);
                        memcpy((void *)pa, &ws, sizeof(ws));
                    }
                }
                tf->a0 = 0; /* success - it IS a terminal */
            } else {
                tf->a0 = (uint64_t)-25; /* -ENOTTY */
            }
        } else if (request == 0x540F) { /* TIOCGPGRP */
            tf->a0 = (uint64_t)current_proc->pid;
        } else if (request == 0x5410) { /* TIOCSPGRP */
            tf->a0 = 0;
        } else {
            tf->a0 = (uint64_t)-25; /* -ENOTTY for unknown ioctls */
        }

    } else if (sysnum == SYS_rt_sigaction) {
        int signum = (int)tf->a0;
        uint64_t act_uaddr = tf->a1;    /* const struct sigaction *act */
        uint64_t oldact_uaddr = tf->a2; /* struct sigaction *oldact */
        /* a3 = sigsetsize, ignored */

        if (signum < 1 || signum >= 32 || signum == 9 || signum == 19) {
            tf->a0 = (uint64_t)-22; /* -EINVAL */
        } else {
            /* Return old action if requested */
            if (oldact_uaddr) {
                struct {
                    uint64_t handler; uint64_t mask;
                    int flags; int pad; uint64_t restorer;
                } old;
                old.handler = current_proc->sigactions[signum].handler;
                old.mask = current_proc->sigactions[signum].mask;
                old.flags = current_proc->sigactions[signum].flags;
                old.pad = 0;
                old.restorer = current_proc->sigactions[signum].restorer;
                pte_t *pte = walk(current_proc->pagetable, oldact_uaddr, 0);
                if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
                    uint64_t pa = PTE2PA(*pte) + (oldact_uaddr & 0xFFF);
                    memcpy((void *)pa, &old, sizeof(old));
                }
            }
            /* Install new action if provided */
            if (act_uaddr) {
                struct {
                    uint64_t handler; uint64_t mask;
                    int flags; int pad; uint64_t restorer;
                } act;
                pte_t *pte = walk(current_proc->pagetable, act_uaddr, 0);
                if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
                    uint64_t pa = PTE2PA(*pte) + (act_uaddr & 0xFFF);
                    memcpy(&act, (void *)pa, sizeof(act));
                    current_proc->sigactions[signum].handler = act.handler;
                    current_proc->sigactions[signum].mask = act.mask;
                    current_proc->sigactions[signum].flags = act.flags;
                    current_proc->sigactions[signum].restorer = act.restorer;
                }
            }
            tf->a0 = 0;
        }

    } else if (sysnum == SYS_rt_sigprocmask) {
        int how = (int)tf->a0;
        uint64_t set_uaddr = tf->a1;
        uint64_t oldset_uaddr = tf->a2;
        /* a3 = sigsetsize, ignored */

        /* Return old mask if requested */
        if (oldset_uaddr) {
            uint64_t oldmask = current_proc->sigmask;
            pte_t *pte = walk(current_proc->pagetable, oldset_uaddr, 0);
            if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
                uint64_t pa = PTE2PA(*pte) + (oldset_uaddr & 0xFFF);
                memcpy((void *)pa, &oldmask, sizeof(oldmask));
            }
        }
        /* Apply new mask if provided */
        if (set_uaddr) {
            uint64_t newset = 0;
            pte_t *pte = walk(current_proc->pagetable, set_uaddr, 0);
            if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
                uint64_t pa = PTE2PA(*pte) + (set_uaddr & 0xFFF);
                memcpy(&newset, (void *)pa, sizeof(newset));
            }
            if (how == 0) /* SIG_BLOCK */
                current_proc->sigmask |= newset;
            else if (how == 1) /* SIG_UNBLOCK */
                current_proc->sigmask &= ~newset;
            else if (how == 2) /* SIG_SETMASK */
                current_proc->sigmask = newset;
        }
        tf->a0 = 0;

    } else if (sysnum == SYS_rt_sigreturn) {
        /* Restore the original trapframe saved on the user stack during
         * signal delivery.  sp points to the saved trapframe. */
        uint64_t usp = tf->sp;

        /* Read saved trapframe from user stack */
        struct trapframe saved;
        memset(&saved, 0, sizeof(saved));
        for (uint64_t off = 0; off < sizeof(struct trapframe); off += 8) {
            pte_t *pte = walk(current_proc->pagetable, usp + off, 0);
            if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
                uint64_t pa = PTE2PA(*pte) + ((usp + off) & 0xFFF);
                *(uint64_t *)((char *)&saved + off) = *(uint64_t *)pa;
            }
        }

        /* Restore the original trapframe */
        memcpy(tf, &saved, sizeof(struct trapframe));
        /* Don't modify tf->a0 -- it was already set in the saved frame.
         * Skip the normal syscall_done return path. */
        return;

    } else if (sysnum == SYS_gettimeofday) {
        uint64_t tv_uaddr = tf->a0;
        /* a1 = timezone pointer, ignored */
        if (tv_uaddr) {
            struct { int64_t sec; int64_t usec; } tv;
            tv.sec = (int64_t)ticks;
            tv.usec = 0;
            pte_t *pte = walk(current_proc->pagetable, tv_uaddr, 0);
            if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
                uint64_t pa = PTE2PA(*pte) + (tv_uaddr & 0xFFF);
                memcpy((void *)pa, &tv, sizeof(tv));
            }
        }
        tf->a0 = 0;

    } else if (sysnum == SYS_fcntl) {
        int fd = (int)tf->a0;
        int cmd = (int)tf->a1;

        if (fd < 0 || fd >= NOFILE || !current_proc->ofile[fd]) {
            tf->a0 = (uint64_t)-9; /* -EBADF */
        } else if (cmd == 1) { /* F_GETFD */
            tf->a0 = 0;
        } else if (cmd == 2) { /* F_SETFD */
            tf->a0 = 0;
        } else if (cmd == 3) { /* F_GETFL */
            tf->a0 = 2; /* O_RDWR */
        } else if (cmd == 4) { /* F_SETFL */
            tf->a0 = 0;
        } else {
            tf->a0 = (uint64_t)-22; /* -EINVAL */
        }

    } else if (sysnum == SYS_faccessat) {
        /* faccessat(dirfd, pathname, mode, flags) */
        const char *path = (const char *)tf->a1;
        if (!path) {
            tf->a0 = (uint64_t)-2; /* -ENOENT */
            goto syscall_done;
        }

        /* Resolve path against cwd */
        char resolved[256];
        resolve_path(current_proc->cwd, path, resolved);

        /* Check via VFS stat */
        const char *relpath;
        struct vfs_ops *ops = vfs_lookup_mount(resolved, &relpath);
        if (ops && ops->stat) {
            uint8_t st[128];
            int ret = ops->stat(relpath, st);
            tf->a0 = (ret == 0) ? 0 : (uint64_t)-2;
        } else {
            tf->a0 = (uint64_t)-2; /* -ENOENT */
        }

    } else if (sysnum == SYS_unlinkat) {
        tf->a0 = (uint64_t)-30; /* -EROFS */

    } else if (sysnum == SYS_linkat) {
        tf->a0 = (uint64_t)-30; /* -EROFS */

    } else if (sysnum == SYS_renameat2) {
        tf->a0 = (uint64_t)-30; /* -EROFS */

    } else if (sysnum == SYS_mkdirat) {
        tf->a0 = (uint64_t)-30; /* -EROFS */

    } else if (sysnum == SYS_umask) {
        tf->a0 = 0022; /* return old umask, always 022 */

    } else {
        printk("Unknown syscall %ld at epc 0x%lx\n", sysnum, tf->epc - size);
        tf->a0 = (uint64_t)-1;
    }
syscall_done:
    /* Deliver pending signals before returning to user mode */
    if (current_proc && (current_proc->sigpending & ~current_proc->sigmask))
        deliver_signal(tf, current_proc);
    return;
}
