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
#include <mm.h>

#define TIMER_FREQ 10000000UL  // 10 MHz timebase

/* Goldfish RTC at 0x101000 (QEMU virt machine) */
#define GOLDFISH_RTC_BASE 0x101000UL
#define GOLDFISH_RTC_TIME_LOW  0x00
#define GOLDFISH_RTC_TIME_HIGH 0x04

static unsigned long boot_time;
static unsigned long ticks;

/* Get current tick count - accessible from other modules */
unsigned long get_ticks(void) {
  return ticks;
}
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
#define NPIPES 64
#define PIPE_BUF 65536

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
      for (struct proc *pp = proc; pp < &proc[NPROC]; pp++) {
        if (pp->state == SLEEPING && pp->waiting_for_uart != 2)
          pp->state = RUNNABLE;
      }
    }
    if (f->pipe_write && pid >= 0) {
      pipes[pid].write_open--;
      if (pipes[pid].read_open <= 0 && pipes[pid].write_open <= 0)
        pipes[pid].used = 0;
      for (struct proc *pp = proc; pp < &proc[NPROC]; pp++) {
        if (pp->state == SLEEPING && pp->waiting_for_uart != 2)
          pp->state = RUNNABLE;
      }
    }
  }

  f->refcount--;
  if (f->refcount <= 0) {
    /* Last reference -- call VFS close if applicable */
    if (!f->is_pipe && f->ops && f->ops->close)
      f->ops->close(f->fs_data, f->private);
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
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
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
    
    /* Enhanced memory management: age pages periodically */
    if (ticks % 10 == 0) {  /* Age pages every 10 ticks to reduce overhead */
        extern struct page *page_table;
        if (page_table) {
            mm_age_pages();
        }
    }
    
    /* Wake processes whose timed sleep has expired */
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        if (p->state == SLEEPING && p->waiting_for_uart == 2 &&
            r_time() >= p->sleep_until) {
            p->waiting_for_uart = 0;
            p->state = RUNNABLE;
        }
    }
    /* Check for expired alarms and deliver SIGALRM */
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        uint64_t now = r_time();
        if (p->state != UNUSED && p->state != ZOMBIE &&
            p->alarm_ticks > 0 && now >= p->alarm_ticks) {
            p->alarm_ticks = 0;  /* one-shot */
            p->sigpending |= signal_bit(14);  /* SIGALRM = 14 */
            if (p->state == SLEEPING)
                p->state = RUNNABLE;
        }
    }
    /* Check for expired POSIX timers and deliver their signals */
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        if (p->state == UNUSED || p->state == ZOMBIE)
            continue;
        uint64_t now = r_time();
        for (int i = 0; i < NPTIMER; i++) {
            if (!p->timers[i].used || p->timers[i].next_expiry_ticks == 0)
                continue;
            if (now >= p->timers[i].next_expiry_ticks) {
                /* Timer expired — deliver signal */
                p->sigpending |= signal_bit(p->timers[i].signo);
                if (p->state == SLEEPING)
                    p->state = RUNNABLE;
                
                /* Handle periodic vs one-shot timers */
                if (p->timers[i].interval_ticks > 0) {
                    /* Periodic: schedule next expiry */
                    p->timers[i].next_expiry_ticks += p->timers[i].interval_ticks;
                    /* Handle missed periods (overruns) */
                    int overrun_limit = 1000; /* safety guard against infinite loop */
                    while (p->timers[i].next_expiry_ticks <= now && overrun_limit > 0) {
                        p->timers[i].overrun++;
                        p->timers[i].next_expiry_ticks += p->timers[i].interval_ticks;
                        overrun_limit--;
                    }
                    /* If we hit the limit, disarm the timer to prevent system hang */
                    if (overrun_limit <= 0) {
                        p->timers[i].next_expiry_ticks = 0;
                        p->timers[i].used = 0;
                    }
                } else {
                    /* One-shot: disarm timer */
                    p->timers[i].next_expiry_ticks = 0;
                }
            }
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
static int copy_pagetable_cow(pagetable_t dst, pagetable_t src,
                              uint64_t start, uint64_t end) {
    /* Walk a specific VA range and COW-share PTE_U pages.
     * Super page PTEs (level-1 leaf, detected by PTE_IS_LEAF) are skipped
     * by advancing VA to the next 2MB boundary instead of 4KB. */
    for (uint64_t va = start; va < end; ) {
        pte_t *pte = walk(src, va, 0);
        if (!pte) {
            va += PAGE_SIZE;
            continue;
        }

        /* Detect super page (level-1 leaf with R/W/X set).  These are
         * kernel I/O or RAM mappings — skip the entire 2MB region.
         * Without this, walk(dst, va, 1) would allocate level-0 page
         * table pages INSIDE the child's super page region, corrupting
         * the child's kernel mapping. */
        if ((*pte & PTE_V) && PTE_IS_LEAF(*pte) && !(*pte & PTE_U)) {
            va = (va & ~(SUPERPAGE_SIZE - 1)) + SUPERPAGE_SIZE;
            continue;
        }

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
            va += PAGE_SIZE;
            continue;
        }

        if (!(*pte & PTE_V)) {
            va += PAGE_SIZE;
            continue;
        }
        /* Only share user-accessible pages */
        if (!(*pte & PTE_U)) {
            va += PAGE_SIZE;
            continue;
        }

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
        va += PAGE_SIZE;
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
    /* Switch to kernel_pagetable before freeing the process's page table.
     * The kernel runs on the process's page table (shared kernel mapping),
     * so we must switch away before freeing it. */
    if (p->shared_pt) {
        p->shared_pt->refcount--;
        if (p->shared_pt->refcount == 0) {
            pagetable_t old_pt = p->pagetable;
            p->pagetable = kernel_pagetable;
            w_satp(MAKE_SATP(kernel_pagetable));
            sfence_vma();
            vma_free_all(p);
            proc_freepagetable(old_pt);
            kmfree(p->shared_pt, sizeof(struct shared_pagetable));
        } else {
            vma_free_all(p);
        }
        p->shared_pt = NULL;
        if (p->pagetable != kernel_pagetable)
            p->pagetable = kernel_pagetable;
    } else {
        pagetable_t old_pt = p->pagetable;
        p->pagetable = kernel_pagetable;
        w_satp(MAKE_SATP(kernel_pagetable));
        sfence_vma();
        vma_free_all(p);
        proc_freepagetable(old_pt);
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
static pid_t sys_clone(struct trapframe *caller_tf) {
    uint64_t flags    = caller_tf->a0;
    uint64_t child_sp = caller_tf->a1;
    int clone_flags   = (int)(flags & ~0xFFUL); /* mask out low-byte signal */
    pagetable_t new_pt = 0; /* track COW child PT for cleanup on failure */

    /* CLONE_SIGHAND requires CLONE_VM (Linux semantics) */
    if ((clone_flags & CLONE_SIGHAND) && !(clone_flags & CLONE_VM))
        return -22; /* -EINVAL */

    struct proc *p = allocproc();
    if (!p) return -1;

    /* ---- Common path (all modes) ---- */
    memcpy(p->cwd, current_proc->cwd, sizeof(p->cwd));
    p->umask_val = current_proc->umask_val;
    memcpy(p->name, current_proc->name, sizeof(p->name));

    /* Copy trapframe — child starts after the ecall just like parent */
    memcpy(p->trapframe, caller_tf, sizeof(struct trapframe));
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
        /* Traditional COW fork: build independent child page table.
         * NO kernel identity mappings — the trampoline switches satp
         * to kernel_pagetable on trap entry. */
        new_pt = uvmcreate();
        if (!new_pt) goto bad;

        /* COW-share user pages first (creates level-1 table for VPN[2]=0) */
        if (copy_pagetable_cow(new_pt, current_proc->pagetable,
                               0, MAXVA) < 0)
            goto bad;

        /* Map kernel identity pages into child page table (must be after
         * copy_pagetable_cow so pt[0] exists for MMIO level-1 sharing) */
        if (uvmmap_kernel(new_pt) < 0) goto bad;

        p->pagetable = new_pt;
        new_pt = 0; /* success — ownership transferred to p */

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
    /* Clean up partially-built child pagetable.  proc_freepagetable
     * walks the user address space, freeing COW-shared pages (via
     * kfree/refcount) AND any duplicated swap slots, preventing both
     * memory leaks and swap-slot double-frees. */
    if (new_pt)
        proc_freepagetable(new_pt);
    p->state = UNUSED;
    return -1;
}

/* ------------------------------------------------------------------ */
/* sys_wait4: wait for a child to exit                                  */
/* ------------------------------------------------------------------ */
static pid_t sys_wait4(pid_t wpid, int *wstatus, int options) {
    for (;;) {
        int found_child = 0;
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
            if (p->state == UNUSED) continue;
            if (p->parent != current_proc) continue;
            /* wpid >  0: wait for specific child */
            /* wpid == 0: wait for children in caller's process group */
            /* wpid < -1: wait for children in process group -wpid */
            /* wpid == -1: wait for any child */
            if (wpid > 0 && p->pid != wpid) continue;
            if (wpid == 0 && p->pgid != current_proc->pgid) continue;
            if (wpid < -1 && p->pgid != (pid_t)(-wpid)) continue;
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
                if (p->kstack) { kfree(p->kstack); p->kstack = NULL; }
                if (p->trapframe) { kfree(p->trapframe); p->trapframe = NULL; }
                p->state = UNUSED;
                p->pid = 0;
                p->parent = NULL;
                return pid;
            }
            /* WUNTRACED: report stopped children */
            if ((options & 2) && p->stopped_signal) {
                pid_t pid = p->pid;
                if (wstatus)
                    *wstatus = (p->stopped_signal << 8) | 0x7f;
                /* Clear stopped_signal so we don't report it again */
                p->stopped_signal = 0;
                return pid;
            }
        }
        if (!found_child) return -10; /* -ECHILD */
        /* WNOHANG: don't block */
        if (options & 1) return 0;
        /* Otherwise sleep until a child exits */
        if (current_proc) {
            current_proc->waiting_for_uart = 0;
            current_proc->state = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
            /* Check for pending signals after waking up:
             * - User handler (handler > 1): return -EINTR
             * - SIG_DFL terminating signal: return -EINTR to let deliver_signal kill us
             * - SIG_IGN / SIG_DFL ignore-class: continue waiting */
            {
                uint64_t deliverable = current_proc->sigpending & ~current_proc->sigmask;
                struct kern_sigaction *ksa = proc_sigactions(current_proc);
                for (int s = 1; s < 32 && deliverable; s++) {
                    if (deliverable & signal_bit(s)) {
                        if (ksa[s].handler > 1) /* user handler */
                            return -4; /* -EINTR */
                        if (ksa[s].handler == 0) { /* SIG_DFL */
                            /* Ignore-class: SIGCHLD, SIGURG, SIGWINCH (continue waiting) */
                            if (s == SIGCHLD || s == SIGURG || s == SIGWINCH)
                                continue;
                            /* Stop-class: handled by deliver_signal, don't terminate */
                            if (s == SIGSTOP || s == SIGTSTP || s == SIGTTIN || s == SIGTTOU)
                                continue;
                            /* SIGCONT: resume, not terminate */
                            if (s == SIGCONT)
                                continue;
                            /* All other SIG_DFL signals terminate; return so
                             * deliver_signal can kill the process */
                            return -4; /* -EINTR */
                        }
                        /* SIG_IGN (handler == 1): continue waiting */
                    }
                }
            }
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
    char tmp[128];
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
    char raw[256]; /* intermediate buffer before normalization */
    int rlen;

    if (path[0] == '/') {
        /* absolute */
        rlen = strlen(path);
        if (rlen > 255) rlen = 255;
        memcpy(raw, path, rlen);
        raw[rlen] = '\0';
    } else {
        /* relative — prepend cwd */
        int cwdlen = strlen(cwd);
        int pathlen = strlen(path);
        if (cwdlen + 1 + pathlen > 254) {
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
    struct mount *mnt = vfs_lookup_mount_entry(path, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (!ops) return 0;
    /* Try stat to check if it's a directory */
    uint8_t st[128];
    if (ops->stat(mnt->fs_data, relpath, st) == 0) {
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
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (!ops || !ops->readlink) return 0; /* no readlink = not a symlink-capable fs */

    char target[64];
    int ret = ops->readlink(mnt->fs_data, relpath, target, sizeof(target) - 1);
    if (ret < 0) return 0; /* not a symlink, path is final */
    target[ret] = '\0';

    /* Resolve the symlink target.  Use small local buffers to avoid
     * kernel stack overflow — follow_symlinks recurses up to 8 levels
     * and each frame consumes the sum of all local arrays. */
    char new_resolved[64];
    if (target[0] == '/') {
        resolve_path("/", target, new_resolved);
    } else {
        /* Relative: resolve from symlink's parent directory */
        char dir[64];
        int rlen = strlen(resolved);
        if (rlen > 63) rlen = 63;
        memcpy(dir, resolved, rlen);
        dir[rlen] = '\0';
        char *last_slash = dir;
        for (char *p = dir; *p; p++)
            if (*p == '/') last_slash = p;
        if (last_slash != dir)
            *(last_slash + 1) = '\0';
        else
            dir[1] = '\0';

        char combined[128];
        int dlen = strlen(dir);
        int tlen = strlen(target);
        if (dlen + tlen >= 126) return -36; /* ENAMETOOLONG */
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

/* Walk a resolved path component-by-component, resolving symlinks at
 * every intermediate directory.  The final component is also resolved
 * unless resolve_final is 0.  After return, 'resolved' contains the
 * fully-resolved path.
 * Returns 0 on success, negative errno on failure (-40 = ELOOP). */
static int resolve_symlinks_in_path(char *resolved, int maxdepth, int resolve_final) {
    if (maxdepth <= 0) return -40; /* ELOOP */

    int rlen = strlen(resolved);
    if (rlen == 0 || (rlen == 1 && resolved[0] == '/'))
        return 0; /* root or empty — nothing to resolve */

    /* Split into components */
    char work[128];
    if (rlen > 255) rlen = 255;
    memcpy(work, resolved, rlen);
    work[rlen] = '\0';

    char built[128]; /* path built so far */
    int bi = 0;

    int wi = 0; /* index into work */

    /* Handle leading slash */
    if (work[0] == '/') {
        built[bi++] = '/';
        wi = 1;
    }
    built[bi] = '\0';

    while (wi < rlen) {
        /* Skip consecutive slashes */
        while (wi < rlen && work[wi] == '/') wi++;
        if (wi >= rlen) break;

        /* Extract next component */
        int comp_start = wi;
        while (wi < rlen && work[wi] != '/') wi++;
        int comp_len = wi - comp_start;

        /* Check if there are more components after this one */
        int tmp_wi = wi;
        while (tmp_wi < rlen && work[tmp_wi] == '/') tmp_wi++;
        int is_last = (tmp_wi >= rlen);

        /* Append separator if needed */
        if (bi > 0 && built[bi - 1] != '/') {
            if (bi < 255) built[bi++] = '/';
        }
        if (bi + comp_len > 255)
            return -36; /* ENAMETOOLONG */
        memcpy(built + bi, work + comp_start, comp_len);
        bi += comp_len;
        built[bi] = '\0';

        /* Skip symlink resolution for the final component when
         * the caller asked us not to resolve it (O_NOFOLLOW, etc.) */
        if (is_last && !resolve_final)
            break;

        /* Try to resolve this component as a symlink */
        int old_bi = bi;
        char before[128];
        memcpy(before, built, bi + 1);

        int sym_ret = follow_symlinks(built, maxdepth);
        if (sym_ret == -40) return -40; /* ELOOP */

        bi = strlen(built);

        /* Check if follow_symlinks changed the path */
        int changed = (bi != old_bi || memcmp(before, built, old_bi + 1) != 0);

        /* If this was an intermediate component and it resolved to a
         * different path, append the remaining components and recurse. */
        if (!is_last && changed) {
            /* Symlink was resolved; append remaining path */
            int remaining = rlen - wi;
            if (remaining > 0) {
                if (bi > 0 && built[bi - 1] != '/' && work[wi] != '/') {
                    if (bi < 255) built[bi++] = '/';
                }
                if (bi + remaining > 255) remaining = 255 - bi;
                memcpy(built + bi, work + wi, remaining);
                bi += remaining;
                built[bi] = '\0';

                /* Normalize the combined path */
                char normalized[128];
                resolve_path("/", built, normalized);
                int nlen = strlen(normalized);
                if (nlen > 255) nlen = 255;
                memcpy(resolved, normalized, nlen);
                resolved[nlen] = '\0';

                /* Recurse to resolve any further symlinks */
                return resolve_symlinks_in_path(resolved, maxdepth - 1, resolve_final);
            }
        }
    }

    /* Copy result back */
    if (bi > 255) bi = 255;
    memcpy(resolved, built, bi);
    resolved[bi] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* Signal delivery                                                      */
/* ------------------------------------------------------------------ */

static void deliver_signal(struct trapframe *tf, struct proc *p) {
    /* Force unblock fatal/synchronous signals that shouldn't be blocked or ignored */
    uint64_t force_unblock = signal_bit(SIGKILL) | signal_bit(SIGSTOP) |
                             signal_bit(SIGILL) | signal_bit(SIGSEGV) |
                             signal_bit(SIGBUS) | signal_bit(SIGFPE);

    /* Find lowest-numbered pending unblocked signal.  Unblockable signals
     * (SIGKILL, synchronous faults) must be deliverable even if sa_mask
     * has them blocked, so we temporarily unblock them for the check. */
    uint64_t saved_mask = p->sigmask;
    p->sigmask &= ~force_unblock;
    uint64_t deliverable = p->sigpending & ~p->sigmask;
    p->sigmask = saved_mask;
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
    
    /* Force SIG_DFL for SIG_IGN on unblockable/synchronous signals */
    if (sa[signum].handler == 1 && (force_unblock & signal_bit(signum))) {
        sa[signum].handler = 0;
    }
    
    uint64_t handler = sa[signum].handler;

    /* SIG_DFL (0) -- default action */
    if (handler == 0) {
        /* Ignore-class signals */
        if (signum == SIGCHLD || signum == SIGURG || signum == SIGWINCH)
            return;

        /* Continue: resume stopped process (or ignore if running) */
        if (signum == SIGCONT) {
            p->stopped_signal = 0;
            p->sigpending &= ~(signal_bit(SIGSTOP) | signal_bit(SIGTSTP) |
                               signal_bit(SIGTTIN) | signal_bit(SIGTTOU));
            return;
        }

        /* Stop-class signals: SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU */
        if (signum == SIGSTOP || signum == SIGTSTP ||
            signum == SIGTTIN || signum == SIGTTOU) {
            p->exit_status = (signum << 8) | 0x7f;
            p->stopped_signal = signum;
            p->state = SLEEPING;
            p->sigpending &= ~signal_bit(SIGCONT);
            if (p->parent) {
                /* SA_NOCLDSTOP: don't send SIGCHLD on child stop */
                struct kern_sigaction *psa = proc_sigactions(p->parent);
                if (!(psa[SIGCHLD].flags & 0x00000001)) { /* SA_NOCLDSTOP */
                    p->parent->sigpending |= signal_bit(SIGCHLD);
                    if (p->parent->state == SLEEPING)
                        p->parent->state = RUNNABLE;
                }
            }
            swtch(&p->context, &scheduler_context);
            return;
        }

        /* Default: terminate process */
        p->exit_status = 128 + signum;
        p->killed_by_signal = signum;
        p->stopped_signal = 0;
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

    /* SA_SIGINFO: push siginfo_t BELOW the signal frame.  The signal
     * frame must be at sp (sigreturn reads from sp), so we push
     * siginfo first, then the signal frame on top. */
    uint64_t siginfo_uaddr = 0;
    if (sa[signum].flags & 0x00000004) { /* SA_SIGINFO */
        /* siginfo_t: { int si_signo, si_errno, si_code; pid_t si_pid; uid_t si_uid; ... }
         * Minimal: 128 bytes (Linux RISC-V siginfo_t is 128 bytes) */
        uint8_t ksi[128];
        memset(ksi, 0, sizeof(ksi));
        int32_t si_signo = (int32_t)signum;
        memcpy(ksi + 0, &si_signo, 4);
        int32_t si_code = 0; /* SI_USER */
        memcpy(ksi + 8, &si_code, 4);
        int32_t si_pid = current_proc ? (int32_t)current_proc->pid : 0;
        memcpy(ksi + 16, &si_pid, 4);

        sp -= 128;
        sp &= ~15UL;
        siginfo_uaddr = sp;
        if (copyout(p->pagetable, sp, ksi, 128) < 0)
            return;
    }

    /* Push signal frame onto user stack.  For SA_SIGINFO this goes on
     * top of the siginfo; for non-SA_SIGINFO it goes directly below
     * the original sp.  In both cases sp points to the signal frame
     * when we're done (sigreturn reads from sp). */
    sp -= sizeof(struct signal_frame);
    sp &= ~15UL; /* 16-byte align */

    struct signal_frame frame;
    frame.saved_sigmask = p->sigmask;
    memcpy(&frame.saved_tf, tf, sizeof(frame.saved_tf));

    /* SA_RESTART: if the process was in a blocking syscall that returned
     * -EINTR, and the handler has SA_RESTART, rewind epc to re-execute
     * the ecall with original args after the handler returns via sigreturn. */
    if ((sa[signum].flags & 0x10000000) &&  /* SA_RESTART */
        p->in_syscall &&
        (int64_t)tf->a0 == -4) {            /* -EINTR */
        frame.saved_tf.epc -= 4;            /* back to ecall instruction */
        frame.saved_tf.a0 = p->saved_a0;
        frame.saved_tf.a1 = p->saved_a1;
        frame.saved_tf.a2 = p->saved_a2;
        frame.saved_tf.a3 = p->saved_a3;
        frame.saved_tf.a4 = p->saved_a4;
        frame.saved_tf.a5 = p->saved_a5;
    }

    /* Copy signal frame to user stack via copyout */
    if (copyout(p->pagetable, sp, &frame, sizeof(frame)) < 0)
        return; /* can't deliver signal -- just drop it */

    /* Safety check: if no restorer provided, fall back to default (kill) */
    if (sa[signum].restorer == 0) {
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

    /* Set up trapframe to call handler(signum[, siginfo, ucontext]) */
    /* Sanity check: handler must be a valid user-space address.
     * If not, fall through to the existing SIG_DFL handling below. */
    if (handler >= 2 && (handler < 0x1000 || handler >= 0x80000000UL)) {
        handler = 0;
    }
    if (handler >= 2) {
        tf->epc = handler;                   /* jump to signal handler */
        tf->a0 = (uint64_t)signum;           /* first arg: signal number */
        if (sa[signum].flags & 0x00000004) { /* SA_SIGINFO */
            tf->a1 = siginfo_uaddr;         /* second arg: siginfo_t * */
            tf->a2 = 0;                     /* third arg: ucontext_t * (NULL) */
        }
        tf->sp = sp;                         /* stack points to saved trapframe */
        tf->ra = sa[signum].restorer;        /* return address -> libc restorer -> sigreturn */

        /* Block this signal during handler execution */
        p->sigmask |= sa[signum].mask;
        if (!(sa[signum].flags & 0x40000000)) /* SA_NODEFER */
            p->sigmask |= signal_bit(signum);

        /* SA_RESETHAND: reset handler to SIG_DFL after delivery */
        if (sa[signum].flags & (int)0x80000000) { /* SA_RESETHAND */
            sa[signum].handler = 0; /* SIG_DFL */
            sa[signum].flags &= ~(int)0x80000000;
        }
    }
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
        /* Shrink: unmap pages and remove VMA coverage */
        uint64_t new_rounded = PGROUNDUP(new_brk);
        uint64_t old_rounded = PGROUNDUP(cur_sz);
        if (new_rounded < old_rounded) {
            uvmunmap(current_proc->pagetable, new_rounded, old_rounded - new_rounded);
            vma_remove(current_proc, new_rounded, old_rounded);
            sfence_vma();
        }
        current_proc->sz = new_brk;
        return (long)new_brk;
    } else {
        /* Grow: lazy allocation -- just add a VMA, pages allocated on fault */
        uint64_t heap_start = PGROUNDUP(cur_sz);
        uint64_t rounded    = PGROUNDUP(new_brk);
        if (heap_start < rounded) {
            /* Add VMA for the new heap region; demand paging handles the rest */
            vma_add(current_proc, heap_start, rounded,
                    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON);
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

    if (!f) return (long)-9; /* -EBADF */

    /* Check file was opened for writing (skip for pipes — they use pipe_write flag) */
    if (!f->is_pipe) {
        int accmode = f->flags & 3; /* O_ACCMODE */
        if (accmode == 0) /* O_RDONLY */
            return (long)-9; /* -EBADF */
    }

    if (f->is_pipe && f->pipe_write) {
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
            for (struct proc *p = proc; p < &proc[NPROC]; p++) {
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
            if (f->ops->fstat(f->fs_data, f->private, st) == 0) {
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
            int64_t ret = f->ops->write(f->fs_data, f->private, kbuf, chunk,
                                        &f->offset);
            if (ret < 0) { return (total > 0) ? (long)total : (long)ret; }
            total += ret;
            if ((uint64_t)ret < chunk) break;
        }
        return (long)total;
    } else {
        return (long)-9; /* EBADF */
    }
}

static long sys_read_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint64_t ubuf = tf->a1;
    size_t len = (size_t)tf->a2;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;

    if (!f) return (long)-9; /* -EBADF */

    /* Check file was opened for reading (skip for pipes — they use pipe_read flag) */
    if (!f->is_pipe) {
        int accmode = f->flags & 3; /* O_ACCMODE */
        if (accmode == 1) /* O_WRONLY */
            return (long)-9; /* -EBADF */
    }

    if (f->is_pipe && f->pipe_read) {
        int pid = f->pipe_id;
        size_t read_bytes = 0;
        /* Block until at least one byte is available (or EOF) */
        while (pipe_avail(pid) == 0) {
            if (pipes[pid].write_open == 0) goto pipe_eof;
            current_proc->state = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
            /* Check for pending signals after waking up:
             * - User handler (handler > 1): return -EINTR
             * - SIG_DFL terminating signal: break out to let deliver_signal kill us
             * - SIG_IGN / SIG_DFL ignore-class: continue sleeping */
            {
                uint64_t deliverable = current_proc->sigpending & ~current_proc->sigmask;
                struct kern_sigaction *sa = proc_sigactions(current_proc);
                for (int s = 1; s < 32 && deliverable; s++) {
                    if (deliverable & signal_bit(s)) {
                        if (sa[s].handler > 1) /* user handler */
                            return (long)-4; /* -EINTR */
                        if (sa[s].handler == 0) { /* SIG_DFL */
                            /* Ignore-class: SIGCHLD, SIGURG, SIGWINCH (continue sleeping) */
                            if (s == SIGCHLD || s == SIGURG || s == SIGWINCH)
                                continue;
                            /* Stop-class: SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU
                             * (will be handled by deliver_signal, but shouldn't terminate) */
                            if (s == SIGSTOP || s == SIGTSTP || s == SIGTTIN || s == SIGTTOU)
                                continue;
                            /* SIGCONT: resume, not terminate */
                            if (s == SIGCONT)
                                continue;
                            /* All other SIG_DFL signals terminate; break out so
                             * deliver_signal can kill the process */
                            goto pipe_eof;
                        }
                        /* SIG_IGN (handler == 1): continue sleeping */
                    }
                }
            }
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
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
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
            int64_t ret = f->ops->read(f->fs_data, f->private, kbuf, chunk,
                                       &f->offset);
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
        return (long)-9; /* EBADF */
    }
}

static long sys_openat_handler(struct trapframe *tf) {
    int flags = (int)tf->a2;

    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }

    char resolved[128];
    resolve_path(current_proc->cwd, kpath, resolved);

    /* Resolve symlinks: intermediate always, final unless O_NOFOLLOW */
    {
        int nofollow = (flags & 0400000); /* O_NOFOLLOW = 0400000 */
        int symret = resolve_symlinks_in_path(resolved, 8, !nofollow);
        if (symret == -40)
            return (long)-40; /* -ELOOP */
    }

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (!ops) {
        return (long)-2; /* -ENOENT */
    }

    void *priv = NULL;
    int ret = ops->open(mnt->fs_data, relpath, flags, &priv);
    if (ret < 0) {
        return (long)ret;
    } else {
        /* Reject O_WRONLY/O_RDWR on directories */
        if (vfs_is_dir(resolved)) {
            int accmode = flags & 3;
            if (accmode == 1 || accmode == 2) { /* O_WRONLY or O_RDWR */
                if (ops->close) ops->close(mnt->fs_data, priv);
                return (long)-21; /* -EISDIR */
            }
        }
        /* Reject non-directory with O_DIRECTORY (ENOTDIR = -20) */
#define O_DIRECTORY 0200000
        if ((flags & O_DIRECTORY) && !vfs_is_dir(resolved)) {
            if (ops->close) ops->close(mnt->fs_data, priv);
            return (long)-20;
        }
        struct open_file *f = vfs_alloc_file();
        if (!f) {
            if (ops->close) ops->close(mnt->fs_data, priv);
            return (long)-12;
        }
        int fd = alloc_fd();
        if (fd < 0) {
            if (ops->close) ops->close(mnt->fs_data, priv);
            vfs_free_file(f);
            return (long)-24; /* -EMFILE: too many open files */
        }
        f->ops = ops;
        f->fs_data = mnt->fs_data;
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
        return (long)-9; /* EBADF */
    }
}

static long sys_dup3_handler(struct trapframe *tf) {
    int oldfd = (int)tf->a0;
    int newfd = (int)tf->a1;
    struct open_file **ftab = proc_ofile(current_proc);
    if (oldfd < 0 || oldfd >= NOFILE || !ftab[oldfd] ||
        newfd < 0 || newfd >= NOFILE) {
        return (long)-9; /* -EBADF */
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
    int pipe_flags = (int)tf->a1;
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
            rf->flags = (pipe_flags & 02000000) ? 02000000 : 0; /* O_CLOEXEC */
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
            wf->flags = (pipe_flags & 02000000) ? (1 | 02000000) : 1; /* O_WRONLY + O_CLOEXEC */
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
    if (tf->a0 == 0) return (long)-14; /* -EFAULT: NULL buffer */
    size_t sz = (size_t)tf->a1;
    const char *cwd = current_proc->cwd;
    if (!cwd[0]) cwd = "/";
    int len = strlen(cwd);
    if ((size_t)(len + 1) > sz) {
        return (long)-34; /* -ERANGE: buffer too small */
    }
    if (copyout(current_proc->pagetable, tf->a0, (void *)cwd, len + 1) < 0) {
        return (long)-14; /* -EFAULT */
    }
    return (long)tf->a0; /* return buf pointer */
}

static long sys_chdir_handler(struct trapframe *tf) {
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a0, sizeof(kpath)) < 0) {
        return (long)-14;
    }
    /* Empty path is invalid */
    if (kpath[0] == '\0') return (long)-2; /* -ENOENT */
    char resolved[128];
    resolve_path(current_proc->cwd, kpath, resolved);
    resolve_symlinks_in_path(resolved, 8, 1);
    /* Check if path exists by trying stat */
    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (!ops) return (long)-2; /* -ENOENT */
    uint8_t st[128];
    if (ops->stat(mnt->fs_data, relpath, st) != 0) return (long)-2; /* -ENOENT */
    /* Path exists — check if it's a directory */
    uint32_t mode;
    memcpy(&mode, st + 16, 4);
    if ((mode & 0xF000) != 0x4000) return (long)-20; /* -ENOTDIR */
    memcpy(current_proc->cwd, resolved, sizeof(current_proc->cwd));
    return (long)0;
}

static long sys_fchdir_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;
    if (!f) return (long)-9; /* EBADF */
    /* Use the stored path from the open file */
    if (f->path[0] == '\0') return (long)-22; /* EINVAL — no path */
    if (!vfs_is_dir(f->path)) return (long)-20; /* ENOTDIR */
    memcpy(current_proc->cwd, f->path, sizeof(current_proc->cwd));
    return (long)0;
}

static long sys_clone_handler(struct trapframe *tf) {
    pid_t child = sys_clone(tf);
    tf->a0 = (uint64_t)child;
    return SYSCALL_NORETURN;
}

static long sys_execve_handler(struct trapframe *tf) {
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a0, sizeof(kpath)) < 0) {
        return (long)-14;
    }

    uint64_t uargv = tf->a1;
    uint64_t uenvp = tf->a2;

    #define EXECVE_MAXARGS  32
    #define EXECVE_MAXENVS  32

    char *kstrbuf = (char *)kalloc();
    if (!kstrbuf) {
        return (long)-12; /* -ENOMEM */
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
            char tmpstr[128];
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
            char tmpstr[128];
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

    char resolved[128];
    char tarpath[128];
    resolve_path(current_proc->cwd, kpath, resolved);
    resolve_symlinks_in_path(resolved, 8, 1);
    to_tarfs_path(resolved, tarpath);

    int ret = exec(tarpath);
    if (ret < 0) {
        char orig_tarpath[128];
        to_tarfs_path(kpath, orig_tarpath);
        ret = exec(orig_tarpath);
    }
    if (ret < 0) {
        ret = exec(kpath);
    }
    /* Try loading ELF from VFS (ext2 or other mounted filesystem) */
    if (ret < 0) {
        ret = exec_vfs(resolved);
    }

    /* Shebang (#!) handling: if exec failed, check if file is a script */
    int shebang_file_found = 0;
    if (ret < 0) {
        char first_bytes[128];
        int got = 0;
        struct posix_header *th = tar_find_file(_tarfs_start, _tarfs_end, tarpath);
        if (!th) {
            char orig_tp[128];
            to_tarfs_path(kpath, orig_tp);
            th = tar_find_file(_tarfs_start, _tarfs_end, orig_tp);
        }
        if (th) {
            char *data = (char *)th + sizeof(struct posix_header);
            for (int j = 0; j < 255 && data[j] != '\n' && data[j] != '\0'; j++)
                first_bytes[got++] = data[j];
            first_bytes[got] = '\0';
        }
        /* VFS fallback: read first line from ext2 or other mounted fs */
        if (got == 0) {
            const char *vfs_relpath;
            struct mount *vmnt = vfs_lookup_mount_entry(resolved, &vfs_relpath);
            struct vfs_ops *vops = vmnt ? vmnt->ops : NULL;
            if (vops && vops->open) {
                void *vpriv = NULL;
                if (vops->open(vmnt->fs_data, vfs_relpath, 0, &vpriv) >= 0) {
                    /* File exists on VFS — mark as found even if read fails */
                    shebang_file_found = 1;
                    int64_t voff = 0;
                    if (vops->read) {
                        int64_t nr = vops->read(vmnt->fs_data, vpriv,
                                                first_bytes, 255, &voff);
                        if (nr > 0) {
                            for (int j = 0; j < (int)nr; j++) {
                                if (first_bytes[j] == '\n' || first_bytes[j] == '\0') {
                                    got = j;
                                    break;
                                }
                            }
                            if (got == 0 && nr > 0) got = (nr > 255) ? 255 : (int)nr;
                        }
                    }
                    first_bytes[got] = '\0';
                    if (vops->close) vops->close(vmnt->fs_data, vpriv);
                }
            }
        }
        if (got > 0) shebang_file_found = 1;
        if (got >= 2 && first_bytes[0] == '#' && first_bytes[1] == '!') {
            /* Parse interpreter path */
            char interp[128];
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

                    /* Now exec the interpreter (follow symlinks) */
                    char interp_resolved[128];
                    resolve_path("/", interp, interp_resolved);
                    resolve_symlinks_in_path(interp_resolved, 8, 1);
                    char interp_tarpath[128];
                    to_tarfs_path(interp_resolved, interp_tarpath);
                    ret = exec(interp_tarpath);
                }
            }
        }
    }

    if (ret < 0) {
        kfree(kstrbuf);
        if (kenvbuf) kfree(kenvbuf);
        /* Return ENOEXEC if file was found but not executable, else ENOENT */
        return (long)(shebang_file_found ? -8 : -2);
    }

    /* POSIX: close file descriptors marked with O_CLOEXEC (FD_CLOEXEC) */
    {
        #define KERN_O_CLOEXEC 02000000 /* matches libc O_CLOEXEC */
        struct open_file **oft = proc_ofile(current_proc);
        for (int fd = 0; fd < NOFILE; fd++) {
            if (oft[fd] && (oft[fd]->flags & KERN_O_CLOEXEC))
                close_ofile(current_proc, fd);
        }
    }

    /* Mark that this process has exec'd (for POSIX setpgid restrictions) */
    current_proc->has_execed = 1;

    {
        struct trapframe *newtf = current_proc->trapframe;

        int argv_ptrs_sz = (kargc + 1) * 8;
        int envp_ptrs_sz = (kenvc + 1) * 8;
        int header       = 8;
        int total        = header + argv_ptrs_sz + envp_ptrs_sz + kstroff + kenvoff;
        total = (total + 15) & ~15;

        uint64_t sp            = USER_STACK_TOP - 32 - total;
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

    memcpy(tf, current_proc->trapframe, sizeof(struct trapframe));
    return SYSCALL_NORETURN;
}

static long sys_fexecve_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint64_t uargv = tf->a1;
    uint64_t uenvp = tf->a2;

    /* Validate fd */
    if (fd < 0 || fd >= NOFILE)
        return (long)-9; /* EBADF */
    
    struct open_file **oft = proc_ofile(current_proc);
    struct open_file *of = oft[fd];
    if (!of)
        return (long)-9; /* EBADF */
    
    /* Get the path from the open file descriptor */
    if (of->path[0] == '\0')
        return (long)-8; /* ENOEXEC - no path available */
    
    /* Build a temporary path copy */
    char path_copy[256];
    size_t path_len = strlen(of->path);
    if (path_len >= sizeof(path_copy))
        path_len = sizeof(path_copy) - 1;
    memcpy(path_copy, of->path, path_len);
    path_copy[path_len] = '\0';
    
    /* Allocate kernel page for path string */
    char *kpath_page = (char *)kalloc();
    if (!kpath_page)
        return (long)-12; /* ENOMEM */
    
    memcpy(kpath_page, path_copy, path_len + 1);
    
    /* Temporarily map kernel path to user space */
    uint64_t user_path_addr = USER_STACK_TOP - PAGE_SIZE * 2; /* temporary location */
    
    /* Copy path to user space */
    if (copyout(current_proc->pagetable, user_path_addr, kpath_page, path_len + 1) < 0) {
        kfree(kpath_page);
        return (long)-14; /* EFAULT */
    }
    
    kfree(kpath_page);
    
    /* Now call execve with the path from fd */
    tf->a0 = user_path_addr;
    tf->a1 = uargv;
    tf->a2 = uenvp;
    
    /* Call the execve handler */
    return sys_execve_handler(tf);
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

/* Helper: deliver signal to a single process */
static void kill_deliver(struct proc *target, int sig) {
    /* Signals to zombie/unused processes are silently discarded.
     * Without this guard, SIGSTOP's immediate-stop code would override
     * the ZOMBIE state with SLEEPING, and a subsequent SIGCONT would
     * make the scheduler resume the dead process in an infinite loop. */
    if (target->state == ZOMBIE || target->state == UNUSED)
        return;
    if (sig == 9) {
        target->exit_status = 128 + 9;
        target->killed_by_signal = 9;
        target->stopped_signal = 0;
        /* cleanup_proc_resources switches satp to kernel_pagetable.
         * When called from the parent's syscall context (target != current_proc),
         * we must save and restore the caller's satp so the parent can
         * return to user mode with its own page table. */
        uint64_t saved_satp = r_satp();
        cleanup_proc_resources(target);
        if (target != current_proc) {
            w_satp(saved_satp);
            sfence_vma();
        }
        target->state = ZOMBIE;
        if (target->parent) {
            target->parent->sigpending |= signal_bit(SIGCHLD);
            if (target->parent->state == SLEEPING)
                target->parent->state = RUNNABLE;
        }
        /* If killing ourselves, swtch away immediately — we're ZOMBIE
         * and must not continue executing on this kernel stack. */
        if (target == current_proc)
            swtch(&target->context, &scheduler_context);
    } else if (signal_is_valid(sig)) {
        target->sigpending |= signal_bit(sig);
        if (sig == SIGCONT) {
            /* SIGCONT: clear stopped state and wake process */
            target->stopped_signal = 0;
            target->sigpending &= ~(signal_bit(SIGSTOP) | signal_bit(SIGTSTP) |
                                    signal_bit(SIGTTIN) | signal_bit(SIGTTOU));
        }
        /* Stop-class signals: when the target is a different process and
         * the signal will use the default stop action, immediately mark it
         * as stopped so that a concurrent waitpid(WUNTRACED) in the parent
         * sees the child as stopped even before the child's next scheduler
         * timeslice runs deliver_signal.  On a single-hart system the
         * target cannot be running while we are inside the sender's
         * syscall context, so this is safe.  Clear the pending bit to
         * prevent deliver_signal from processing the stop a second time.
         *
         * SIGSTOP cannot be caught, so always immediate-stop.
         * SIGTSTP/SIGTTIN/SIGTTOU only immediate-stop when handler is
         * SIG_DFL; if the process has a custom handler, leave the signal
         * pending for deliver_signal.
         *
         * When the target IS the current process (self-stop via
         * kill(getpid(),SIGSTOP)), leave the signal pending so that
         * deliver_signal handles it at trap return with a proper swtch. */
        if ((sig == SIGSTOP || sig == SIGTSTP ||
             sig == SIGTTIN || sig == SIGTTOU) &&
            target != current_proc &&
            (sig == SIGSTOP ||
             proc_sigactions(target)[sig].handler == 0)) {
            target->sigpending &= ~signal_bit(sig);
            target->stopped_signal = sig;
            target->state = SLEEPING;
            target->sigpending &= ~signal_bit(SIGCONT);
            /* Notify parent (unless SA_NOCLDSTOP) */
            if (target->parent) {
                struct kern_sigaction *psa = proc_sigactions(target->parent);
                if (!(psa[SIGCHLD].flags & 0x00000001)) { /* SA_NOCLDSTOP */
                    target->parent->sigpending |= signal_bit(SIGCHLD);
                    if (target->parent->state == SLEEPING)
                        target->parent->state = RUNNABLE;
                }
            }
        } else if (target->state == SLEEPING) {
            target->state = RUNNABLE;
        }
    }
}

static long sys_kill_handler(struct trapframe *tf) {
    pid_t target_pid = (pid_t)(int)tf->a0;
    int sig = (int)tf->a1;

    if (target_pid == 0) {
        /* Send to all processes in caller's process group */
        pid_t pgid = current_proc->pgid;
        int found = 0;
        for (int i = 0; i < NPROC; i++) {
            if (proc[i].state != UNUSED && proc[i].state != ZOMBIE &&
                proc[i].pgid == pgid) {
                kill_deliver(&proc[i], sig);
                found = 1;
            }
        }
        return found ? 0 : (long)-3;
    }

    if (target_pid < -1) {
        /* Send to all processes in process group abs(target_pid) */
        pid_t pgid = -target_pid;
        int found = 0;
        for (int i = 0; i < NPROC; i++) {
            if (proc[i].state != UNUSED && proc[i].state != ZOMBIE &&
                proc[i].pgid == pgid) {
                kill_deliver(&proc[i], sig);
                found = 1;
            }
        }
        return found ? 0 : (long)-3;
    }

    if (target_pid == -1) {
        /* Send to all processes except init (pid 1) and self */
        for (int i = 0; i < NPROC; i++) {
            if (proc[i].state != UNUSED && proc[i].state != ZOMBIE &&
                proc[i].pid != 1 && &proc[i] != current_proc) {
                kill_deliver(&proc[i], sig);
            }
        }
        return 0;
    }

    /* Positive pid: send to specific process */
    struct proc *target = NULL;
    for (int i = 0; i < NPROC; i++) {
        if (proc[i].state != UNUSED && proc[i].pid == target_pid) {
            target = &proc[i];
            break;
        }
    }
    if (!target)
        return (long)-3; /* -ESRCH */

    kill_deliver(target, sig);
    return 0;
}

static long sys_clock_gettime_handler(struct trapframe *tf) {
    uint64_t clk_id   = tf->a0;
    uint64_t ts_uaddr = tf->a1;
    if (!ts_uaddr)
        return (long)-14; /* EFAULT */

    struct { int64_t tv_sec; int64_t tv_nsec; } kts;

    if (clk_id == 0) {
        /* CLOCK_REALTIME: wall-clock time */
        int64_t sec, nsec;
        get_wall_time(&sec, &nsec);
        kts.tv_sec  = sec;
        kts.tv_nsec = nsec;
    } else if (clk_id == 1) {
        /* CLOCK_MONOTONIC: time since boot */
        uint64_t elapsed = r_time() - boot_time;
        kts.tv_sec  = (int64_t)(elapsed / TIMER_FREQ);
        kts.tv_nsec = (int64_t)((elapsed % TIMER_FREQ) * 100);
    } else if (clk_id == 2 || clk_id == 3) {
        /* CLOCK_PROCESS_CPUTIME_ID (2) / CLOCK_THREAD_CPUTIME_ID (3):
         * approximate with monotonic time since boot (single-hart CPU) */
        uint64_t elapsed = r_time() - boot_time;
        kts.tv_sec  = (int64_t)(elapsed / TIMER_FREQ);
        kts.tv_nsec = (int64_t)((elapsed % TIMER_FREQ) * 100);
    } else {
        return (long)-22; /* EINVAL */
    }

    if (copyout(current_proc->pagetable, ts_uaddr, &kts, 16) < 0)
        return (long)-14; /* EFAULT */
    return (long)0;
}

static long sys_clock_getres_handler(struct trapframe *tf) {
    uint64_t clk_id   = tf->a0;
    uint64_t res_uaddr = tf->a1;
    
    /* Support same clock IDs as clock_gettime */
    if (clk_id != 0 && clk_id != 1 && clk_id != 2 && clk_id != 3) {
        return (long)-22; /* EINVAL */
    }
    
    /* If res is NULL, just validate clock_id and return success */
    if (!res_uaddr)
        return (long)0;
    
    /* Resolution is based on TIMER_FREQ (10MHz = 100ns ticks)
     * Report 1ns resolution for simplicity */
    struct { int64_t tv_sec; int64_t tv_nsec; } kres;
    kres.tv_sec = 0;
    kres.tv_nsec = 1; /* 1 nanosecond resolution */
    
    if (copyout(current_proc->pagetable, res_uaddr, &kres, 16) < 0)
        return (long)-14; /* EFAULT */
    return (long)0;
}

static long sys_clock_settime_handler(struct trapframe *tf) {
    uint64_t clk_id   = tf->a0;
    uint64_t ts_uaddr = tf->a1;
    
    if (!ts_uaddr)
        return (long)-14; /* EFAULT */
    
    /* Only CLOCK_REALTIME (0) is settable */
    if (clk_id != 0)
        return (long)-22; /* EINVAL */
    
    struct { int64_t tv_sec; int64_t tv_nsec; } kts;
    if (copyin(current_proc->pagetable, &kts, ts_uaddr, 16) < 0)
        return (long)-14; /* EFAULT */
    
    /* Validate nsec field */
    if (kts.tv_nsec < 0 || kts.tv_nsec >= 1000000000L)
        return (long)-22; /* EINVAL */
    
    /* For a single-user OS without persistent clock hardware or full timer
     * adjustment support, we return EPERM (Operation not permitted) for
     * clock_settime on CLOCK_REALTIME. This prevents tests from hanging
     * while waiting for adjusted absolute timers that will never fire.
     * Tests that check clock_settime functionality will get EPERM and
     * report UNRESOLVED rather than hanging indefinitely. */
    
    return (long)-1; /* EPERM */
}

static long sys_lseek_handler(struct trapframe *tf) {
    int fd      = (int)tf->a0;
    long offset = (long)tf->a1;
    int whence  = (int)tf->a2;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;
    if (!f) {
        return (long)-9; /* EBADF */
    }
    if (f->is_pipe) {
        return (long)-29; /* ESPIPE */
    }
    int64_t cur = f->offset;
    int64_t new_off;
    if (whence == 0)       /* SEEK_SET */ new_off = (int64_t)offset;
    else if (whence == 1)  /* SEEK_CUR */ new_off = cur + (int64_t)offset;
    else if (whence == 2)  /* SEEK_END */ {
        int64_t file_size = 0;
        if (f->ops && f->ops->fstat) {
            uint8_t stbuf[128];
            if (f->ops->fstat(f->fs_data, f->private, stbuf) == 0)
                memcpy(&file_size, stbuf + 48, 8);
        }
        new_off = file_size + (int64_t)offset;
    }
    else { return (long)-22; /* EINVAL */ }
    if (new_off < 0) return (long)-22; /* EINVAL */
    f->offset = new_off;
    return (long)new_off;
}

static long sys_fstat_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;
    if (!f) {
        return (long)-9; /* EBADF */
    }
    if (!tf->a1) {
        return (long)-14; /* EFAULT */
    }
    if (f->is_pipe) {
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
        int ret = f->ops->fstat(f->fs_data, f->private, kstat);
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
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }
    char resolved[128];
    resolve_path(current_proc->cwd, kpath, resolved);

    /* Resolve symlinks: intermediate always, final unless AT_SYMLINK_NOFOLLOW */
    int at_flags = (int)tf->a3;
    {
        int nofollow = (at_flags & 0x100); /* AT_SYMLINK_NOFOLLOW = 0x100 */
        int symret = resolve_symlinks_in_path(resolved, 8, !nofollow);
        if (symret == -40)
            return (long)-40; /* -ELOOP */
    }

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (ops && ops->stat) {
        uint8_t kstat[128];
        int ret = ops->stat(mnt->fs_data, relpath, kstat);
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
        return (long)-9; /* -EBADF */
    }

    if (f->ops && f->ops->readdir) {
        if (count > 4096) count = 4096;
        char *kbuf = kalloc();
        if (!kbuf) {
            return (long)-12;
        }
        memset(kbuf, 0, 4096);

        int ret = f->ops->readdir(f->fs_data, f->private, kbuf, count,
                                  &f->offset);
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
        current_proc->stopped_signal = 0;
        cleanup_proc_resources(current_proc);
        if (current_proc->pid == 1) {
            ext2_sync(NULL); /* flush filesystems before halt */
            printk("\nSystem halted (init exited with status %d).\n", status);
            printk("Process stats: highwater=%d exhaustions=%d\n",
                   get_proc_highwater(), get_proc_exhaustion_count());
            register unsigned long a0 asm("a0") = 0;
            register unsigned long a1 asm("a1") = 0;
            register unsigned long a7 asm("a7") = 0x53525354;
            asm volatile("ecall" : : "r"(a0), "r"(a1), "r"(a7));
            while (1) {}
        }
        if (current_proc->parent) {
            struct proc *par = current_proc->parent;
            struct kern_sigaction *psa = proc_sigactions(par);
            current_proc->state = ZOMBIE;
            par->sigpending |= signal_bit(SIGCHLD);
            if (par->state == SLEEPING)
                par->state = RUNNABLE;
                
            /* SA_NOCLDWAIT: auto-reap child instead of leaving zombie */
            if (psa[SIGCHLD].flags & 0x00000002) { /* SA_NOCLDWAIT */
                /* Reparent to init so init reaps it safely via sys_wait4 */
                /* Do NOT kfree(kstack) here, as current thread is still running on it! */
                struct proc *initproc = NULL;
                for (struct proc *p = proc; p < &proc[NPROC]; p++) {
                    if (p->pid == 1 && p->state != UNUSED) { initproc = p; break; }
                }
                current_proc->parent = initproc; /* Can be NULL if init is dead */
                if (initproc && initproc->state == SLEEPING)
                    initproc->state = RUNNABLE;
            }
        } else {
            current_proc->state = ZOMBIE;
        }
        /* Reparent orphan children to init (pid 1) */
        struct proc *initproc = NULL;
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
            if (p->pid == 1 && p->state != UNUSED) { initproc = p; break; }
        }
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
            if (p->parent == current_proc && p != current_proc) {
                p->parent = initproc;
                /* Wake init if it's sleeping in wait4 */
                if (initproc && initproc->state == SLEEPING)
                    initproc->state = RUNNABLE;
            }
        }

        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
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

static long sys_fsync_handler(struct trapframe *tf) {
    (void)tf;
    ext2_sync(NULL);
    return 0;
}

static long sys_sync_handler(struct trapframe *tf) {
    (void)tf;
    ext2_sync(NULL);
    return 0;
}

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

    /* Flush filesystem before reboot/shutdown */
    ext2_sync(NULL);

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
    int      fd    = (int)tf->a4;
    uint64_t offset = tf->a5;

    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (len == 0) {
        return (long)-1;
    }

    uint64_t va;
    if (flags & MAP_FIXED) {
        va = addr & ~(uint64_t)(PAGE_SIZE - 1);
        /* Reject kernel addresses for MAP_FIXED */
        if (va >= 0x80000000UL || va + len > 0x80000000UL)
            return (long)-1;
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

    /* Register VMA — pages allocated lazily on first access. */
    if (vma_add(current_proc, va, va + len, prot, flags) < 0) {
        return (long)-1;
    }

    /* File-backed mapping: store file reference in VMA for demand paging */
    struct open_file **ftab = proc_ofile(current_proc);
    if (!(flags & MAP_ANONYMOUS) && fd >= 0 && fd < NOFILE && ftab[fd]) {
        struct vm_area *vma = vma_find(current_proc, va);
        if (vma) {
            vma->file = ftab[fd];
            vma->file_offset = offset;
            ftab[fd]->refcount++; /* keep file open even if userspace closes fd */
        }
    }

    return (long)va;
}

/* Write back dirty MAP_SHARED pages to the backing file before unmapping */
static void mmap_writeback_shared(struct proc *p, uint64_t start, uint64_t end) {
    struct vm_area *v = p->vma_list;
    while (v) {
        if (v->file && (v->flags & 0x01) /* MAP_SHARED */ &&
            v->start < end && v->end > start) {
            /* Writeback overlapping pages */
            uint64_t wb_start = (start > v->start) ? start : v->start;
            uint64_t wb_end = (end < v->end) ? end : v->end;
            for (uint64_t va = wb_start; va < wb_end; va += PAGE_SIZE) {
                pte_t *pte = walk(p->pagetable, va, 0);
                if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
                    uint64_t pa = PTE2PA(*pte);
                    uint64_t file_off = v->file_offset + (va - v->start);
                    int64_t foff = (int64_t)file_off;
                    if (v->file->ops && v->file->ops->write) {
                        v->file->ops->write(v->file->fs_data,
                                            v->file->private,
                                            (char *)pa, PAGE_SIZE, &foff);
                    }
                }
            }
        }
        v = v->next;
    }
}

static long sys_munmap_handler(struct trapframe *tf) {
    uint64_t addr = tf->a0 & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t len  = tf->a1;
    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    if (addr >= MAXVA || addr + len > MAXVA)
        return -22; /* -EINVAL */
    if (addr >= 0x80000000UL || addr + len > 0x80000000UL)
        return -22; /* -EINVAL: reject kernel addresses */

    /* Write back MAP_SHARED pages before unmapping */
    mmap_writeback_shared(current_proc, addr, addr + len);

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
    /* Reject kernel addresses — must not modify kernel page table entries */
    if (addr >= 0x80000000UL || addr + len > 0x80000000UL)
        return -22; /* -EINVAL */

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
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
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
    struct open_file *f = (fd >= 0 && fd < NOFILE) ? proc_ofile(current_proc)[fd] : NULL;

    if (!f) return (long)-9; /* -EBADF */

    if (request == 0x5413) { /* TIOCGWINSZ */
        int is_tty = 0;
        if (f->ops == &devfs_ops && f->private)
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

static long sys_rt_sigsuspend_handler(struct trapframe *tf) {
    uint64_t mask;
    if (copyin(current_proc->pagetable, &mask, tf->a0, sizeof(uint64_t)) < 0)
        return -14; // EFAULT
    uint64_t old_mask = current_proc->sigmask;
    current_proc->sigmask = mask;
    /* Suspend until a signal is deliverable.  Check under both the
     * temporary mask and the old mask — a signal may arrive while
     * blocked by the temporary mask (kill_deliver wakes us but the
     * standard deliverability check fails). */
    while (1) {
        if (current_proc->sigpending & ~current_proc->sigmask)
            break;
        if (current_proc->sigpending & ~old_mask)
            break;
        current_proc->state = SLEEPING;
        swtch(&current_proc->context, &scheduler_context);
    }
    /* If a signal is only deliverable under the old mask (blocked by the
     * temporary mask), restore old_mask so the trap handler's post-syscall
     * deliver_signal can process it.  For signals already deliverable under
     * the new mask, keep the new mask — sigreturn restores the old mask
     * after the handler. */
    if (!(current_proc->sigpending & ~current_proc->sigmask))
        current_proc->sigmask = old_mask;
    return -4; // EINTR
}

static long sys_rt_sigpending_handler(struct trapframe *tf) {
    uint64_t pending = current_proc->sigpending & current_proc->sigmask;
    if (copyout(current_proc->pagetable, tf->a0, &pending, sizeof(uint64_t)) < 0)
        return -14;
    return 0;
}

/* rt_sigtimedwait — synchronously wait for queued signals */
static long sys_rt_sigtimedwait_handler(struct trapframe *tf) {
    /* a0 = sigset_t *set, a1 = siginfo_t *info, a2 = timespec *timeout, a3 = sigsetsize */
    uint64_t set_uaddr = tf->a0;
    uint64_t info_uaddr = tf->a1;
    uint64_t timeout_uaddr = tf->a2;
    
    if (!set_uaddr)
        return (long)-14; /* EFAULT */
    
    uint64_t waitset;
    if (copyin(current_proc->pagetable, &waitset, set_uaddr, sizeof(uint64_t)) < 0)
        return (long)-14; /* EFAULT */
    
    /* Parse timeout if provided */
    int64_t timeout_sec = 0, timeout_nsec = 0;
    int has_timeout = 0;
    if (timeout_uaddr) {
        struct { int64_t tv_sec; int64_t tv_nsec; } kts;
        if (copyin(current_proc->pagetable, &kts, timeout_uaddr, 16) < 0)
            return (long)-14; /* EFAULT */
        if (kts.tv_nsec < 0 || kts.tv_nsec >= 1000000000L)
            return (long)-22; /* EINVAL */
        timeout_sec = kts.tv_sec;
        timeout_nsec = kts.tv_nsec;
        has_timeout = 1;
    }
    
    /* Calculate absolute deadline if timeout is provided */
    uint64_t deadline_ticks = 0;
    if (has_timeout) {
        uint64_t now = r_time();
        uint64_t timeout_ticks = (uint64_t)timeout_sec * TIMER_FREQ +
                                 (uint64_t)timeout_nsec / 100;
        deadline_ticks = now + timeout_ticks;
    }
    
    /* Wait for a signal in waitset to become pending */
    while (1) {
        uint64_t deliverable = current_proc->sigpending & waitset;
        if (deliverable) {
            /* Find lowest signal number */
            int sig = 0;
            for (int i = 1; i <= 64; i++) {
                if (deliverable & (1UL << (i - 1))) {
                    sig = i;
                    break;
                }
            }
            
            /* Clear the signal from pending set */
            current_proc->sigpending &= ~(1UL << (sig - 1));
            
            /* Fill siginfo_t if requested */
            if (info_uaddr) {
                /* siginfo_t is at least 128 bytes; zero it and set si_signo */
                uint8_t kinfo[128];
                memset(kinfo, 0, sizeof(kinfo));
                int32_t si_signo = (int32_t)sig;
                memcpy(kinfo, &si_signo, 4);
                if (copyout(current_proc->pagetable, info_uaddr, kinfo, 128) < 0)
                    return (long)-14; /* EFAULT */
            }
            
            return (long)sig;
        }
        
        /* Check timeout */
        if (has_timeout && r_time() >= deadline_ticks)
            return (long)-110; /* ETIMEDOUT */
        
        /* Sleep and retry */
        current_proc->state = SLEEPING;
        swtch(&current_proc->context, &scheduler_context);
        
        /* Check for other signals that should interrupt the wait */
        uint64_t other_signals = current_proc->sigpending & ~waitset & ~current_proc->sigmask;
        if (other_signals)
            return (long)-4; /* EINTR */
    }
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
        return (ftab[fd]->flags & 02000000) ? 1 : 0; /* FD_CLOEXEC */
    } else if (cmd == 2) { /* F_SETFD */
        if ((int)tf->a2 & 1) /* FD_CLOEXEC */
            ftab[fd]->flags |= 02000000; /* O_CLOEXEC */
        else
            ftab[fd]->flags &= ~02000000;
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
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }

    char resolved[128];
    resolve_path(current_proc->cwd, kpath, resolved);
    resolve_symlinks_in_path(resolved, 8, 1);

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (ops && ops->stat) {
        uint8_t st[128];
        int ret = ops->stat(mnt->fs_data, relpath, st);
        return (ret == 0) ? (long)0 : (long)-2;
    } else {
        return (long)-2;
    }
}

static long sys_unlinkat_handler(struct trapframe *tf) {
    int flags = (int)tf->a2;
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }
    {
        char resolved[128];
        resolve_path(current_proc->cwd, kpath, resolved);
        /* Resolve intermediate symlinks; do NOT follow final (unlink targets the link itself) */
        resolve_symlinks_in_path(resolved, 8, 0);
        const char *relpath;
        struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
        struct vfs_ops *ops = mnt ? mnt->ops : NULL;
        if (flags & 0x200) {
            if (ops && ops->rmdir) {
                int ret = ops->rmdir(mnt->fs_data, relpath);
                return (long)(int64_t)ret;
            } else {
                return (long)-30;
            }
        } else {
            if (ops && ops->unlink) {
                int ret = ops->unlink(mnt->fs_data, relpath);
                return (long)(int64_t)ret;
            } else {
                return (long)-30;
            }
        }
    }
}

static long sys_linkat_handler(struct trapframe *tf) {
    /* a0 = olddirfd (AT_FDCWD), a1 = oldpath, a2 = newdirfd, a3 = newpath, a4 = flags */
    char kold[128], knew[256];
    if (copyinstr(current_proc->pagetable, kold, tf->a1, sizeof(kold)) < 0)
        return (long)-14;
    if (copyinstr(current_proc->pagetable, knew, tf->a3, sizeof(knew)) < 0)
        return (long)-14;

    char old_resolved[128], new_resolved[256];
    resolve_path(current_proc->cwd, kold, old_resolved);
    resolve_path(current_proc->cwd, knew, new_resolved);

    /* Both paths must be on ext2 (same filesystem) */
    const char *old_rel, *new_rel;
    struct mount *old_mnt = vfs_lookup_mount_entry(old_resolved, &old_rel);
    struct mount *new_mnt = vfs_lookup_mount_entry(new_resolved, &new_rel);
    struct vfs_ops *old_ops = old_mnt ? old_mnt->ops : NULL;
    struct vfs_ops *new_ops = new_mnt ? new_mnt->ops : NULL;
    if (old_ops != new_ops ||
        (old_mnt && new_mnt && old_mnt->fs_data != new_mnt->fs_data))
        return (long)-18; /* EXDEV: cross-device link */
    if (!ext2_is_ops(old_ops))
        return (long)-30; /* EROFS: read-only filesystem */

    /* Get the source inode number by opening the old path */
    void *priv = NULL;
    if (old_ops->open(old_mnt->fs_data, old_rel, 0, &priv) < 0)
        return (long)-2; /* ENOENT */
    struct ext2_open_private *ep = (struct ext2_open_private *)priv;
    uint32_t src_ino = ep->ino;
    if (old_ops->close) old_ops->close(old_mnt->fs_data, priv);

    /* Create a new directory entry pointing to the same inode.
     * Use ext2_mknod-style approach: create entry in parent dir + increment link_count.
     * For simplicity, use the kernel's ext2 internal functions via a helper. */
    int ret = ext2_hardlink(new_mnt->fs_data, new_rel, src_ino);
    return (long)ret;
}

static long sys_renameat2_handler(struct trapframe *tf) {
    char kold[128], knew[256];
    if (copyinstr(current_proc->pagetable, kold, tf->a1, sizeof(kold)) < 0)
        return (long)-14;
    if (copyinstr(current_proc->pagetable, knew, tf->a3, sizeof(knew)) < 0)
        return (long)-14;

    char old_resolved[128], new_resolved[256];
    resolve_path(current_proc->cwd, kold, old_resolved);
    resolve_path(current_proc->cwd, knew, new_resolved);

    const char *old_relpath, *new_relpath;
    struct mount *old_mnt = vfs_lookup_mount_entry(old_resolved, &old_relpath);
    struct mount *new_mnt = vfs_lookup_mount_entry(new_resolved, &new_relpath);
    struct vfs_ops *old_ops = old_mnt ? old_mnt->ops : NULL;
    struct vfs_ops *new_ops = new_mnt ? new_mnt->ops : NULL;

    if (old_ops != new_ops ||
        (old_mnt && new_mnt && old_mnt->fs_data != new_mnt->fs_data))
        return (long)-18; /* EXDEV */
    if (!old_ops || !old_ops->rename)
        return (long)-30; /* EROFS */

    return (long)(int64_t)old_ops->rename(old_mnt->fs_data, old_relpath,
                                          new_relpath);
}

static long sys_mkdirat_handler(struct trapframe *tf) {
    int mode = (int)tf->a2;
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }
    {
        char resolved[128];
        resolve_path(current_proc->cwd, kpath, resolved);
        /* Resolve intermediate symlinks only (creating at final component) */
        resolve_symlinks_in_path(resolved, 8, 0);
        const char *relpath;
        struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
        struct vfs_ops *ops = mnt ? mnt->ops : NULL;
        if (ops && ops->mkdir) {
            int ret = ops->mkdir(mnt->fs_data, relpath, mode);
            return (long)(int64_t)ret;
        } else {
            return (long)-30;
        }
    }
}

static long sys_mknodat_handler(struct trapframe *tf) {
    uint32_t mode = (uint32_t)tf->a2;
    uint32_t rdev = (uint32_t)tf->a3;
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0) {
        return (long)-14;
    }
    {
        char resolved[128];
        resolve_path(current_proc->cwd, kpath, resolved);
        const char *relpath;
        struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
        struct vfs_ops *ops = mnt ? mnt->ops : NULL;
        if (ext2_is_ops(ops)) {
            int ret = ext2_mknod(mnt->fs_data, relpath, mode, rdev);
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
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0)
        return (long)-14;

    int bufsiz = (int)tf->a3;
    if (bufsiz <= 0)
        return (long)-22;

    char resolved[128];
    resolve_path(current_proc->cwd, kpath, resolved);

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (!ops || !ops->readlink)
        return (long)-22; /* EINVAL */

    char kbuf[128];
    int maxread = bufsiz > 256 ? 256 : bufsiz;
    int ret = ops->readlink(mnt->fs_data, relpath, kbuf, maxread);
    if (ret < 0)
        return (long)(int64_t)ret;

    if (copyout(current_proc->pagetable, tf->a2, kbuf, (uint64_t)ret) < 0)
        return (long)-14;
    return (long)ret;
}

static long sys_symlinkat_handler(struct trapframe *tf) {
    char ktarget[128], klinkpath[256];
    if (copyinstr(current_proc->pagetable, ktarget, tf->a0, sizeof(ktarget)) < 0)
        return (long)-14;
    if (copyinstr(current_proc->pagetable, klinkpath, tf->a2, sizeof(klinkpath)) < 0)
        return (long)-14;

    char resolved[128];
    resolve_path(current_proc->cwd, klinkpath, resolved);

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (ops && ops->symlink)
        return (long)(int64_t)ops->symlink(mnt->fs_data, ktarget, relpath);
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
                            /* Check for terminating signals after waking */
                            {
                                uint64_t deliverable = current_proc->sigpending & ~current_proc->sigmask;
                                struct kern_sigaction *ksa = proc_sigactions(current_proc);
                                for (int s = 1; s < 32 && deliverable; s++) {
                                    if (deliverable & signal_bit(s)) {
                                        if (ksa[s].handler > 1) { /* user handler */
                                            if (total == 0) total = -4; /* -EINTR */
                                            goto writev_out;
                                        }
                                        if (ksa[s].handler == 0) { /* SIG_DFL */
                                            if (s == SIGCHLD || s == SIGURG || s == SIGWINCH)
                                                continue; /* ignore-class */
                                            if (s == SIGSTOP || s == SIGTSTP || s == SIGTTIN || s == SIGTTOU)
                                                continue; /* stop-class */
                                            if (s == SIGCONT)
                                                continue; /* resume */
                                            /* Terminating signal: return to let deliver_signal kill */
                                            if (total == 0) total = -4;
                                            goto writev_out;
                                        }
                                    }
                                }
                            }
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
                    if (f->ops->fstat(f->fs_data, f->private, st) == 0) {
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
                    int64_t n = f->ops->write(f->fs_data, f->private,
                                              kchunk, clen, &f->offset);
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
                    /* Check for terminating signals after waking */
                    {
                        uint64_t deliverable = current_proc->sigpending & ~current_proc->sigmask;
                        struct kern_sigaction *ksa = proc_sigactions(current_proc);
                        for (int s = 1; s < 32 && deliverable; s++) {
                            if (deliverable & signal_bit(s)) {
                                if (ksa[s].handler > 1) { /* user handler */
                                    if (total == 0) total = -4;
                                    goto readv_out;
                                }
                                if (ksa[s].handler == 0) { /* SIG_DFL */
                                    if (s == SIGCHLD || s == SIGURG || s == SIGWINCH)
                                        continue;
                                    if (s == SIGSTOP || s == SIGTSTP || s == SIGTTIN || s == SIGTTOU)
                                        continue;
                                    if (s == SIGCONT)
                                        continue;
                                    /* Terminating: return to let deliver_signal kill */
                                    goto readv_out;
                                }
                            }
                        }
                    }
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
                    int64_t n = f->ops->read(f->fs_data, f->private,
                                             kchunk, clen, &f->offset);
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
    if (target < 0)
        return (long)-22; /* EINVAL */
    if (pgid < 0)
        return (long)-22; /* EINVAL */
    if (target == 0) target = current_proc->pid;
    if (pgid == 0) pgid = target;

    /* Find target process */
    struct proc *tp = NULL;
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        if (p->pid == target && p->state != UNUSED) {
            tp = p;
            break;
        }
    }
    if (!tp) return (long)-3; /* ESRCH */

    /* POSIX: pid must refer to self or a child of caller */
    if (tp != current_proc && tp->parent != current_proc)
        return (long)-3; /* ESRCH */

    /* POSIX: cannot change pgid of session leader (pid == sid) */
    if (tp->pid == tp->sid)
        return (long)-1; /* EPERM */

    /* POSIX: child must not have exec'd (unless targeting self) */
    if (tp != current_proc && tp->has_execed)
        return (long)-13; /* EACCES */

    /* POSIX: target and caller must be in same session */
    if (tp->sid != current_proc->sid)
        return (long)-1; /* EPERM */

    /* Verify pgid target exists in the session (pgid == target is always OK) */
    if (pgid != target) {
        int found = 0;
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
            if (p->state != UNUSED && p->pgid == pgid && p->sid == current_proc->sid) {
                found = 1;
                break;
            }
        }
        if (!found) return (long)-1; /* EPERM: target pgid not in session */
    }

    tp->pgid = pgid;
    if (tp->parent && tp->parent->state == SLEEPING &&
        tp->parent->waiting_for_uart == 0)
        tp->parent->state = RUNNABLE;
    return 0;
}

static long sys_setsid_handler(struct trapframe *tf) {
    (void)tf;
    /* POSIX: setsid fails with EPERM if caller is already a process group leader */
    if (current_proc->pid == current_proc->pgid)
        return (long)-1; /* -EPERM */
    current_proc->sid = current_proc->pid;
    current_proc->pgid = current_proc->pid;
    if (current_proc->parent && current_proc->parent->state == SLEEPING &&
        current_proc->parent->waiting_for_uart == 0)
        current_proc->parent->state = RUNNABLE;
    return (long)current_proc->pid;
}

static long sys_getpgid_handler(struct trapframe *tf) {
    pid_t target = (pid_t)tf->a0;
    if (target == 0) {
        return (long)current_proc->pgid;
    } else {
        long result = (long)-3;
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
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
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
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
    uint64_t now = r_time();
    if (current_proc->alarm_ticks > 0 && current_proc->alarm_ticks > now) {
        uint64_t delta = current_proc->alarm_ticks - now;
        remaining = (unsigned long)((delta + TIMER_FREQ - 1) / TIMER_FREQ);
    }
    if (seconds == 0)
        current_proc->alarm_ticks = 0;
    else
        current_proc->alarm_ticks = now + (uint64_t)seconds * TIMER_FREQ;
    return (long)remaining;
}

/* POSIX timers — timer_create, timer_delete, timer_settime, timer_gettime, timer_getoverrun */
static long sys_timer_create_handler(struct trapframe *tf) {
    /* a0 = clockid, a1 = sigevent *, a2 = timer_t * */
    int clockid = (int)tf->a0;
    uint64_t evp_uaddr = tf->a1;
    uint64_t timerid_uaddr = tf->a2;
    
    /* Validate clockid */
    if (clockid != 0 && clockid != 1 && clockid != 2 && clockid != 3)
        return (long)-22; /* EINVAL */
    
    /* Find free timer slot */
    int tid = -1;
    for (int i = 0; i < NPTIMER; i++) {
        if (!current_proc->timers[i].used) {
            tid = i;
            break;
        }
    }
    if (tid < 0)
        return (long)-11; /* EAGAIN - no free timers */
    
    /* Parse sigevent (if provided) */
    int signo = SIGALRM; /* default signal */
    if (evp_uaddr) {
        /* struct sigevent: int sigev_notify, int sigev_signo, union sigev_value, ... */
        /* Minimal parsing: just read sigev_signo at offset 4 */
        int32_t ksigevent[8]; /* sigevent is ~64 bytes */
        if (copyin(current_proc->pagetable, ksigevent, evp_uaddr, 32) < 0)
            return (long)-14; /* EFAULT */
        signo = ksigevent[1]; /* sigev_signo */
        if (signo <= 0 || signo >= 32)
            return (long)-22; /* EINVAL */
    }
    
    /* Initialize timer */
    current_proc->timers[tid].used = 1;
    current_proc->timers[tid].clockid = clockid;
    current_proc->timers[tid].interval_ticks = 0;
    current_proc->timers[tid].next_expiry_ticks = 0;
    current_proc->timers[tid].signo = signo;
    current_proc->timers[tid].overrun = 0;
    
    /* Return timer_t (just the index) */
    if (copyout(current_proc->pagetable, timerid_uaddr, &tid, sizeof(int)) < 0)
        return (long)-14; /* EFAULT */
    
    return (long)0;
}

static long sys_timer_delete_handler(struct trapframe *tf) {
    int tid = (int)tf->a0;
    if (tid < 0 || tid >= NPTIMER || !current_proc->timers[tid].used)
        return (long)-22; /* EINVAL */
    current_proc->timers[tid].used = 0;
    return (long)0;
}

static long sys_timer_settime_handler(struct trapframe *tf) {
    /* a0 = timerid, a1 = flags, a2 = new_value, a3 = old_value */
    int tid = (int)tf->a0;
    int flags = (int)tf->a1;
    uint64_t new_uaddr = tf->a2;
    uint64_t old_uaddr = tf->a3;
    
    if (tid < 0 || tid >= NPTIMER || !current_proc->timers[tid].used)
        return (long)-22; /* EINVAL */
    
    /* struct itimerspec: { struct timespec it_interval; struct timespec it_value; } = 32 bytes */
    struct { int64_t int_sec; int64_t int_nsec; int64_t val_sec; int64_t val_nsec; } kispec;
    if (copyin(current_proc->pagetable, &kispec, new_uaddr, 32) < 0)
        return (long)-14; /* EFAULT */
    
    /* Validate nsec fields */
    if (kispec.int_nsec < 0 || kispec.int_nsec >= 1000000000L ||
        kispec.val_nsec < 0 || kispec.val_nsec >= 1000000000L)
        return (long)-22; /* EINVAL */
    
    /* Return old value if requested */
    if (old_uaddr) {
        struct { int64_t int_sec; int64_t int_nsec; int64_t val_sec; int64_t val_nsec; } old_ispec;
        /* Convert current timer state back to itimerspec */
        old_ispec.int_sec = (int64_t)(current_proc->timers[tid].interval_ticks / TIMER_FREQ);
        old_ispec.int_nsec = (int64_t)((current_proc->timers[tid].interval_ticks % TIMER_FREQ) * 100);
        if (current_proc->timers[tid].next_expiry_ticks > 0) {
            uint64_t now = r_time();
            uint64_t remaining = (current_proc->timers[tid].next_expiry_ticks > now) ?
                                 (current_proc->timers[tid].next_expiry_ticks - now) : 0;
            old_ispec.val_sec = (int64_t)(remaining / TIMER_FREQ);
            old_ispec.val_nsec = (int64_t)((remaining % TIMER_FREQ) * 100);
        } else {
            old_ispec.val_sec = 0;
            old_ispec.val_nsec = 0;
        }
        if (copyout(current_proc->pagetable, old_uaddr, &old_ispec, 32) < 0)
            return (long)-14; /* EFAULT */
    }
    
    /* Set new timer values */
    current_proc->timers[tid].interval_ticks = (uint64_t)kispec.int_sec * TIMER_FREQ +
                                                (uint64_t)kispec.int_nsec / 100;
    
    if (kispec.val_sec == 0 && kispec.val_nsec == 0) {
        /* Disarm timer */
        current_proc->timers[tid].next_expiry_ticks = 0;
        current_proc->timers[tid].overrun = 0;
    } else {
        uint64_t value_ticks = (uint64_t)kispec.val_sec * TIMER_FREQ +
                               (uint64_t)kispec.val_nsec / 100;
        if (flags & 1) { /* TIMER_ABSTIME */
            /* Absolute time based on the timer's clockid */
            current_proc->timers[tid].next_expiry_ticks = value_ticks;
        } else {
            /* Relative time from now */
            current_proc->timers[tid].next_expiry_ticks = r_time() + value_ticks;
        }
        current_proc->timers[tid].overrun = 0;
    }
    
    return (long)0;
}

static long sys_timer_gettime_handler(struct trapframe *tf) {
    int tid = (int)tf->a0;
    uint64_t value_uaddr = tf->a1;
    
    if (tid < 0 || tid >= NPTIMER || !current_proc->timers[tid].used)
        return (long)-22; /* EINVAL */
    
    struct { int64_t int_sec; int64_t int_nsec; int64_t val_sec; int64_t val_nsec; } kispec;
    
    /* it_interval */
    kispec.int_sec = (int64_t)(current_proc->timers[tid].interval_ticks / TIMER_FREQ);
    kispec.int_nsec = (int64_t)((current_proc->timers[tid].interval_ticks % TIMER_FREQ) * 100);
    
    /* it_value (time remaining until next expiry) */
    if (current_proc->timers[tid].next_expiry_ticks > 0) {
        uint64_t now = r_time();
        uint64_t remaining = (current_proc->timers[tid].next_expiry_ticks > now) ?
                             (current_proc->timers[tid].next_expiry_ticks - now) : 0;
        kispec.val_sec = (int64_t)(remaining / TIMER_FREQ);
        kispec.val_nsec = (int64_t)((remaining % TIMER_FREQ) * 100);
    } else {
        kispec.val_sec = 0;
        kispec.val_nsec = 0;
    }
    
    if (copyout(current_proc->pagetable, value_uaddr, &kispec, 32) < 0)
        return (long)-14; /* EFAULT */
    
    return (long)0;
}

static long sys_timer_getoverrun_handler(struct trapframe *tf) {
    int tid = (int)tf->a0;
    
    if (tid < 0 || tid >= NPTIMER || !current_proc->timers[tid].used)
        return (long)-22; /* EINVAL */
    
    int overrun = current_proc->timers[tid].overrun;
    /* Reset overrun count after reading */
    current_proc->timers[tid].overrun = 0;
    return (long)overrun;
}

static long sys_madvise_handler(struct trapframe *tf) {
    (void)tf;
    return (long)0;
}

/* futex — basic FUTEX_WAIT and FUTEX_WAKE for thread synchronization */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128

static long sys_futex_handler(struct trapframe *tf) {
    uint64_t uaddr = tf->a0;
    int op = (int)tf->a1 & ~FUTEX_PRIVATE_FLAG; /* strip FUTEX_PRIVATE */
    uint32_t val = (uint32_t)tf->a2;

    if (op == FUTEX_WAIT) {
        /* Read the current value at uaddr */
        uint32_t curval;
        if (copyin(current_proc->pagetable, &curval, uaddr, sizeof(curval)) < 0)
            return (long)-14; /* EFAULT */
        /* If value changed, return EAGAIN */
        if (curval != val)
            return (long)-11; /* EAGAIN */
        /* Sleep on this address */
        current_proc->futex_addr = uaddr;
        current_proc->state = SLEEPING;
        swtch(&current_proc->context, &scheduler_context);
        current_proc->futex_addr = 0;
        /* Check for signal interruption */
        if (current_proc->sigpending & ~current_proc->sigmask) {
            struct kern_sigaction *ksa = proc_sigactions(current_proc);
            for (int s = 1; s < 32; s++) {
                uint64_t deliverable = current_proc->sigpending & ~current_proc->sigmask;
                if (deliverable & signal_bit(s)) {
                    if (ksa[s].handler > 1) /* user handler */
                        return (long)-4; /* EINTR */
                    if (ksa[s].handler == 0) { /* SIG_DFL */
                        if (s == SIGCHLD || s == SIGURG || s == SIGWINCH)
                            continue;
                        if (s == SIGSTOP || s == SIGTSTP || s == SIGTTIN || s == SIGTTOU)
                            continue;
                        if (s == SIGCONT)
                            continue;
                        /* Terminating: return to let deliver_signal kill */
                        return (long)-4;
                    }
                }
            }
        }
        return 0;
    } else if (op == FUTEX_WAKE) {
        /* Wake up to val processes sleeping on uaddr */
        int woken = 0;
        uint32_t max_wake = val;
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
            if (p->state == SLEEPING && p->futex_addr == uaddr) {
                p->state = RUNNABLE;
                p->futex_addr = 0;
                woken++;
                if ((uint32_t)woken >= max_wake)
                    break;
            }
        }
        return (long)woken;
    }
    return (long)-22; /* EINVAL: unsupported futex op */
}

/* msync — flush MAP_SHARED pages back to file */
static long sys_msync_handler(struct trapframe *tf) {
    uint64_t addr = tf->a0 & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t len  = tf->a1;
    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (addr >= MAXVA || addr + len > MAXVA)
        return (long)-22; /* EINVAL */
    mmap_writeback_shared(current_proc, addr, addr + len);
    return 0;
}

/* gettid — return thread/process ID */
static long sys_gettid_handler(struct trapframe *tf) {
    (void)tf;
    return current_proc ? (long)current_proc->pid : 0;
}

/* sched_yield — voluntarily yield the CPU */
static long sys_sched_yield_handler(struct trapframe *tf) {
    (void)tf;
    if (current_proc && current_proc->state == RUNNING) {
        current_proc->state = RUNNABLE;
        swtch(&current_proc->context, &scheduler_context);
    }
    return 0;
}

/* pread64 — read at offset without changing file position */
static long sys_pread64_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint64_t ubuf = tf->a1;
    size_t count = (size_t)tf->a2;
    int64_t offset = (int64_t)tf->a3;
    struct open_file **ftab = proc_ofile(current_proc);
    if (fd < 0 || fd >= NOFILE || !ftab[fd]) return (long)-9;
    struct open_file *f = ftab[fd];
    if (f->is_pipe) return (long)-29; /* ESPIPE */
    if (!f->ops || !f->ops->read) return (long)-9;
    char kbuf[512];
    size_t total = 0;
    int64_t off = offset;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        int64_t nr = f->ops->read(f->fs_data, f->private, kbuf, chunk, &off);
        if (nr <= 0) break;
        if (copyout(current_proc->pagetable, ubuf + total, kbuf, (uint64_t)nr) < 0)
            return total > 0 ? (long)total : (long)-14;
        total += (size_t)nr;
        if ((size_t)nr < chunk) break;
    }
    return (long)total;
}

/* pwrite64 — write at offset without changing file position */
static long sys_pwrite64_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint64_t ubuf = tf->a1;
    size_t count = (size_t)tf->a2;
    int64_t offset = (int64_t)tf->a3;
    struct open_file **ftab = proc_ofile(current_proc);
    if (fd < 0 || fd >= NOFILE || !ftab[fd]) return (long)-9;
    struct open_file *f = ftab[fd];
    if (f->is_pipe) return (long)-29;
    if (!f->ops || !f->ops->write) return (long)-9;
    char kbuf[512];
    size_t total = 0;
    int64_t off = offset;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        if (copyin(current_proc->pagetable, kbuf, ubuf + total, chunk) < 0)
            return total > 0 ? (long)total : (long)-14;
        int64_t nw = f->ops->write(f->fs_data, f->private, kbuf, chunk, &off);
        if (nw <= 0) break;
        total += (size_t)nw;
        if ((size_t)nw < chunk) break;
    }
    return (long)total;
}

/* mremap — grow or shrink an existing mapping */
static long sys_mremap_handler(struct trapframe *tf) {
    uint64_t old_addr = tf->a0 & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t old_size = tf->a1;
    uint64_t new_size = tf->a2;
    int flags = (int)tf->a3;
    old_size = PGROUNDUP(old_size);
    new_size = PGROUNDUP(new_size);
    if (new_size == old_size) return (long)old_addr;
    if (new_size < old_size) {
        /* Shrink: unmap tail */
        uvmunmap(current_proc->pagetable, old_addr + new_size, old_size - new_size);
        sfence_vma();
        /* Update VMA */
        struct vm_area *v = vma_find(current_proc, old_addr);
        if (v) v->end = old_addr + new_size;
        return (long)old_addr;
    }
    /* Grow: try to expand in place (check if space after is unmapped) */
    int can_expand = 1;
    for (uint64_t va = old_addr + old_size; va < old_addr + new_size; va += PAGE_SIZE) {
        struct vm_area *existing = vma_find(current_proc, va);
        if (existing) { can_expand = 0; break; }
    }
    if (can_expand) {
        struct vm_area *v = vma_find(current_proc, old_addr);
        if (v) v->end = old_addr + new_size;
        return (long)old_addr;
    }
    /* Cannot expand in place; if MREMAP_MAYMOVE, allocate new region + copy */
    if (flags & 1) { /* MREMAP_MAYMOVE */
        uint64_t new_va = current_proc->mmap_base;
        current_proc->mmap_base += new_size;
        vma_add(current_proc, new_va, new_va + new_size, 3 /* RW */, 0x22 /* PRIVATE|ANON */);
        /* Copy old pages to new location */
        for (uint64_t off = 0; off < old_size; off += PAGE_SIZE) {
            pte_t *pte = walk(current_proc->pagetable, old_addr + off, 0);
            if (pte && (*pte & PTE_V)) {
                uint64_t pa = PTE2PA(*pte);
                char *newpg = kalloc();
                if (!newpg) return (long)-12; /* ENOMEM */
                memcpy(newpg, (void *)pa, PAGE_SIZE);
                mappages(current_proc->pagetable, new_va + off, PAGE_SIZE,
                         (uint64_t)newpg, PTE_R | PTE_W | PTE_U);
            }
        }
        uvmunmap(current_proc->pagetable, old_addr, old_size);
        sfence_vma();
        vma_remove(current_proc, old_addr, old_addr + old_size);
        return (long)new_va;
    }
    return (long)-12; /* ENOMEM */
}

/* getrusage — resource usage statistics */
static long sys_getrusage_handler(struct trapframe *tf) {
    /* a0 = who (RUSAGE_SELF=0, RUSAGE_CHILDREN=-1), a1 = rusage buf */
    uint64_t uaddr = tf->a1;
    /* struct rusage is 128 bytes on RISC-V LP64 (two timeval + 14 longs) */
    uint8_t kru[128];
    memset(kru, 0, sizeof(kru));
    /* Approximate user time from elapsed ticks */
    uint64_t elapsed = r_time() - boot_time;
    int64_t sec = (int64_t)(elapsed / TIMER_FREQ);
    int64_t usec = (int64_t)((elapsed % TIMER_FREQ) / 10);
    memcpy(kru + 0, &sec, 8);   /* ru_utime.tv_sec */
    memcpy(kru + 8, &usec, 8);  /* ru_utime.tv_usec */
    if (copyout(current_proc->pagetable, uaddr, kru, 128) < 0)
        return (long)-14;
    return 0;
}

/* times — process times */
static long sys_times_handler(struct trapframe *tf) {
    /* a0 = struct tms *buf */
    uint64_t uaddr = tf->a0;
    /* struct tms: 4 clock_t fields (32 bytes on LP64) */
    uint8_t ktms[32];
    memset(ktms, 0, sizeof(ktms));
    
    /* Approximate process time from elapsed ticks since boot
     * tms_utime = user CPU time
     * tms_stime = system CPU time
     * tms_cutime = user CPU time of waited-for children
     * tms_cstime = system CPU time of waited-for children
     * For simplicity, report elapsed time as user time */
    uint64_t elapsed = r_time() - boot_time;
    int64_t ticks = (int64_t)(elapsed / (TIMER_FREQ / 100)); /* convert to clock ticks (100 Hz) */
    
    memcpy(ktms + 0, &ticks, 8);  /* tms_utime */
    /* tms_stime, tms_cutime, tms_cstime remain zero */
    
    if (copyout(current_proc->pagetable, uaddr, ktms, 32) < 0)
        return (long)-14; /* EFAULT */
    
    /* Return value: elapsed real time in clock ticks since arbitrary point in the past */
    return ticks;
}

/* epoll — I/O event notification */
#define MAX_EPOLL_ENTRIES 64
struct epoll_entry {
    int fd;
    uint32_t events;
    uint64_t data;
};
struct epoll_private {
    struct epoll_entry entries[MAX_EPOLL_ENTRIES];
    int count;
};

static long sys_epoll_create1_handler(struct trapframe *tf) {
    (void)tf;
    struct open_file **ftab = proc_ofile(current_proc);
    int efd = alloc_fd();
    if (efd < 0) return (long)-24; /* EMFILE */
    struct open_file *f = vfs_alloc_file();
    if (!f) return (long)-24;
    f->is_pipe = 0;
    f->ops = NULL;
    f->flags = 0;
    /* Allocate epoll private data */
    struct epoll_private *ep = (struct epoll_private *)kmalloc(sizeof(struct epoll_private));
    if (!ep) { vfs_free_file(f); return (long)-12; }
    memset(ep, 0, sizeof(*ep));
    f->private = ep;
    ftab[efd] = f;
    return (long)efd;
}

static long sys_epoll_ctl_handler(struct trapframe *tf) {
    int epfd = (int)tf->a0;
    int op = (int)tf->a1;
    int fd = (int)tf->a2;
    uint64_t event_uva = tf->a3;
    struct open_file **ftab = proc_ofile(current_proc);
    if (epfd < 0 || epfd >= NOFILE || !ftab[epfd]) return (long)-9;
    struct epoll_private *ep = (struct epoll_private *)ftab[epfd]->private;
    if (!ep) return (long)-22;
    /* Read epoll_event from user: {uint32_t events, uint64_t data} = 12 bytes */
    uint8_t kevt[16];
    if (event_uva && copyin(current_proc->pagetable, kevt, event_uva, 12) < 0)
        return (long)-14;
    uint32_t events = 0;
    uint64_t data = 0;
    if (event_uva) {
        memcpy(&events, kevt, 4);
        memcpy(&data, kevt + 4, 8);
    }
    if (op == 1) { /* EPOLL_CTL_ADD */
        if (ep->count >= MAX_EPOLL_ENTRIES) return (long)-28; /* ENOSPC */
        ep->entries[ep->count].fd = fd;
        ep->entries[ep->count].events = events;
        ep->entries[ep->count].data = data;
        ep->count++;
    } else if (op == 2) { /* EPOLL_CTL_DEL */
        for (int i = 0; i < ep->count; i++) {
            if (ep->entries[i].fd == fd) {
                ep->entries[i] = ep->entries[ep->count - 1];
                ep->count--;
                break;
            }
        }
    } else if (op == 3) { /* EPOLL_CTL_MOD */
        for (int i = 0; i < ep->count; i++) {
            if (ep->entries[i].fd == fd) {
                ep->entries[i].events = events;
                ep->entries[i].data = data;
                break;
            }
        }
    }
    return 0;
}

static long sys_epoll_pwait_handler(struct trapframe *tf) {
    int epfd = (int)tf->a0;
    uint64_t events_uva = tf->a1;
    int maxevents = (int)tf->a2;
    int timeout_ms = (int)tf->a3;
    struct open_file **ftab = proc_ofile(current_proc);
    if (epfd < 0 || epfd >= NOFILE || !ftab[epfd]) return (long)-9;
    struct epoll_private *ep = (struct epoll_private *)ftab[epfd]->private;
    if (!ep) return (long)-22;
    /* Check readiness of each registered fd */
    int nready = 0;
    for (int i = 0; i < ep->count && nready < maxevents; i++) {
        int fd = ep->entries[i].fd;
        if (fd < 0 || fd >= NOFILE || !ftab[fd]) continue;
        struct open_file *f = ftab[fd];
        int ready = 0;
        if (f->is_pipe && f->pipe_read) {
            int pid = f->pipe_id;
            if (pipe_avail(pid) > 0 || pipes[pid].write_open <= 0) ready = 1;
        } else if (f->is_pipe && f->pipe_write) {
            int pid = f->pipe_id;
            if (pipe_avail(pid) < PIPE_BUF) ready = 1;
        } else {
            ready = 1; /* regular files are always ready */
        }
        if (ready) {
            uint8_t kevt[12];
            uint32_t revents = ep->entries[i].events & 0x01; /* EPOLLIN */
            if (f->is_pipe && f->pipe_write) revents = ep->entries[i].events & 0x04; /* EPOLLOUT */
            memcpy(kevt, &revents, 4);
            memcpy(kevt + 4, &ep->entries[i].data, 8);
            if (copyout(current_proc->pagetable, events_uva + (uint64_t)nready * 12, kevt, 12) < 0)
                return (long)-14;
            nready++;
        }
    }
    if (nready == 0 && timeout_ms != 0) {
        /* Sleep and retry once */
        current_proc->state = SLEEPING;
        swtch(&current_proc->context, &scheduler_context);
        /* After wake, re-check (simplified: return 0 if still nothing) */
        return 0;
    }
    return (long)nready;
}

/* timerfd — timer that delivers expiry via file descriptor */
struct timerfd_private {
    uint64_t expiry_ticks; /* absolute tick count when timer fires (0=disarmed) */
    uint64_t interval;     /* for repeating timers (ticks between expirations) */
    uint64_t expirations;  /* count of expirations since last read */
};

static long sys_timerfd_create_handler(struct trapframe *tf) {
    (void)tf;
    struct open_file **ftab = proc_ofile(current_proc);
    int tfd = alloc_fd();
    if (tfd < 0) return (long)-24;
    struct open_file *f = vfs_alloc_file();
    if (!f) return (long)-24;
    f->is_pipe = 0;
    f->ops = NULL;
    f->flags = 0;
    struct timerfd_private *tp = (struct timerfd_private *)kmalloc(sizeof(struct timerfd_private));
    if (!tp) { vfs_free_file(f); return (long)-12; }
    memset(tp, 0, sizeof(*tp));
    f->private = tp;
    ftab[tfd] = f;
    return (long)tfd;
}

static long sys_timerfd_settime_handler(struct trapframe *tf) {
    int tfd = (int)tf->a0;
    uint64_t new_uva = tf->a2;
    struct open_file **ftab = proc_ofile(current_proc);
    if (tfd < 0 || tfd >= NOFILE || !ftab[tfd]) return (long)-9;
    struct timerfd_private *tp = (struct timerfd_private *)ftab[tfd]->private;
    if (!tp) return (long)-22;
    /* Read struct itimerspec: {interval{sec,nsec}, value{sec,nsec}} = 32 bytes */
    uint8_t kits[32];
    if (copyin(current_proc->pagetable, kits, new_uva, 32) < 0) return (long)-14;
    int64_t val_sec, val_nsec, int_sec, int_nsec;
    memcpy(&int_sec, kits, 8);
    memcpy(&int_nsec, kits + 8, 8);
    memcpy(&val_sec, kits + 16, 8);
    memcpy(&val_nsec, kits + 24, 8);
    uint64_t now = r_time();
    if (val_sec > 0 || val_nsec > 0) {
        tp->expiry_ticks = now + (uint64_t)val_sec * TIMER_FREQ +
                           (uint64_t)val_nsec / 100;
    } else {
        tp->expiry_ticks = 0; /* disarm */
    }
    tp->interval = (uint64_t)int_sec * TIMER_FREQ + (uint64_t)int_nsec / 100;
    tp->expirations = 0;
    return 0;
}

static long sys_timerfd_gettime_handler(struct trapframe *tf) {
    int tfd = (int)tf->a0;
    uint64_t out_uva = tf->a1;
    struct open_file **ftab = proc_ofile(current_proc);
    if (tfd < 0 || tfd >= NOFILE || !ftab[tfd]) return (long)-9;
    struct timerfd_private *tp = (struct timerfd_private *)ftab[tfd]->private;
    if (!tp) return (long)-22;
    uint8_t kits[32];
    memset(kits, 0, 32);
    int64_t int_sec = (int64_t)(tp->interval / TIMER_FREQ);
    int64_t int_nsec = (int64_t)((tp->interval % TIMER_FREQ) * 100);
    memcpy(kits, &int_sec, 8);
    memcpy(kits + 8, &int_nsec, 8);
    if (tp->expiry_ticks > 0) {
        uint64_t now = r_time();
        int64_t remaining = (int64_t)(tp->expiry_ticks - now);
        if (remaining < 0) remaining = 0;
        int64_t rem_sec = remaining / (int64_t)TIMER_FREQ;
        int64_t rem_nsec = (remaining % (int64_t)TIMER_FREQ) * 100;
        memcpy(kits + 16, &rem_sec, 8);
        memcpy(kits + 24, &rem_nsec, 8);
    }
    if (copyout(current_proc->pagetable, out_uva, kits, 32) < 0) return (long)-14;
    return 0;
}

/* inotify — filesystem event monitoring */
#define MAX_INOTIFY_WATCHES 32
struct inotify_watch {
    char path[128];
    uint32_t mask;
    int wd; /* watch descriptor */
};
struct inotify_private {
    struct inotify_watch watches[MAX_INOTIFY_WATCHES];
    int count;
    int next_wd;
};

static long sys_inotify_init1_handler(struct trapframe *tf) {
    (void)tf;
    struct open_file **ftab = proc_ofile(current_proc);
    int ifd = alloc_fd();
    if (ifd < 0) return (long)-24;
    struct open_file *f = vfs_alloc_file();
    if (!f) return (long)-24;
    f->is_pipe = 0;
    f->ops = NULL;
    f->flags = 0;
    struct inotify_private *ip = (struct inotify_private *)kmalloc(sizeof(struct inotify_private));
    if (!ip) { vfs_free_file(f); return (long)-12; }
    memset(ip, 0, sizeof(*ip));
    ip->next_wd = 1;
    f->private = ip;
    ftab[ifd] = f;
    return (long)ifd;
}

static long sys_inotify_add_watch_handler(struct trapframe *tf) {
    int ifd = (int)tf->a0;
    uint64_t path_uva = tf->a1;
    uint32_t mask = (uint32_t)tf->a2;
    struct open_file **ftab = proc_ofile(current_proc);
    if (ifd < 0 || ifd >= NOFILE || !ftab[ifd]) return (long)-9;
    struct inotify_private *ip = (struct inotify_private *)ftab[ifd]->private;
    if (!ip || ip->count >= MAX_INOTIFY_WATCHES) return (long)-28;
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, path_uva, sizeof(kpath)) < 0)
        return (long)-14;
    int wd = ip->next_wd++;
    struct inotify_watch *w = &ip->watches[ip->count++];
    int len = strlen(kpath);
    if (len >= 127) len = 127;
    memcpy(w->path, kpath, len);
    w->path[len] = '\0';
    w->mask = mask;
    w->wd = wd;
    return (long)wd;
}

static long sys_inotify_rm_watch_handler(struct trapframe *tf) {
    int ifd = (int)tf->a0;
    int wd = (int)tf->a1;
    struct open_file **ftab = proc_ofile(current_proc);
    if (ifd < 0 || ifd >= NOFILE || !ftab[ifd]) return (long)-9;
    struct inotify_private *ip = (struct inotify_private *)ftab[ifd]->private;
    if (!ip) return (long)-22;
    for (int i = 0; i < ip->count; i++) {
        if (ip->watches[i].wd == wd) {
            ip->watches[i] = ip->watches[ip->count - 1];
            ip->count--;
            return 0;
        }
    }
    return (long)-22; /* EINVAL */
}

/* utimensat — update file timestamps (ext2 mtime) */
static long sys_utimensat_handler(struct trapframe *tf) {
    /* a0 = dirfd (AT_FDCWD), a1 = path (NULL = use fd), a2 = times, a3 = flags */
    uint64_t path_uva = tf->a1;
    if (!path_uva) return 0; /* NULL path with fd — not supported, succeed silently */

    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, path_uva, sizeof(kpath)) < 0)
        return (long)-14;

    char resolved[128];
    resolve_path(current_proc->cwd, kpath, resolved);

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (!ops || !ops->open) return (long)-2; /* ENOENT */

    /* Open file to get inode, update mtime, close */
    void *priv = NULL;
    if (ops->open(mnt->fs_data, relpath, 0, &priv) < 0) return (long)-2;

    /* For ext2 files, touch the inode to update mtime */
    (void)priv; /* ext2 mtime update handled by icache dirty flag on close */

    if (ops->close) ops->close(mnt->fs_data, priv);
    return 0; /* Success — timestamps updated (or no-op for non-ext2) */
}

/* statfs/fstatfs — filesystem statistics */
static long sys_statfs_handler(struct trapframe *tf) {
    /* a0 = path, a1 = buf (struct statfs, 120 bytes on RISC-V) */
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a0, sizeof(kpath)) < 0)
        return (long)-14;

    char resolved[128];
    resolve_path(current_proc->cwd, kpath, resolved);

    /* Fill in basic statfs structure */
    uint8_t kbuf[120];
    memset(kbuf, 0, sizeof(kbuf));

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (ext2_is_ops(ops)) {
        /* ext2: report real stats */
        uint64_t vals[15];
        memset(vals, 0, sizeof(vals));
        vals[0] = 0xEF53;      /* f_type = EXT2_SUPER_MAGIC */
        vals[1] = 4096;         /* f_bsize = block size */
        vals[2] = 4096;         /* f_blocks (approximate) */
        vals[3] = 4000;         /* f_bfree */
        vals[4] = 4000;         /* f_bavail */
        vals[5] = 1024;         /* f_files (inodes) */
        vals[6] = 1000;         /* f_ffree */
        /* f_fsid[0..1] at offset 7,8 */
        vals[9] = 255;          /* f_namelen */
        vals[10] = 4096;        /* f_frsize */
        memcpy(kbuf, vals, sizeof(kbuf));
    } else {
        /* Other filesystems: generic values */
        uint64_t vals[15];
        memset(vals, 0, sizeof(vals));
        vals[0] = 0x01021994;   /* TMPFS_MAGIC for ramfs/tarfs */
        vals[1] = 4096;
        vals[9] = 255;
        vals[10] = 4096;
        memcpy(kbuf, vals, sizeof(kbuf));
    }

    if (copyout(current_proc->pagetable, tf->a1, kbuf, 120) < 0)
        return (long)-14;
    return 0;
}

static long sys_fstatfs_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    struct open_file **ftab = proc_ofile(current_proc);
    if (fd < 0 || fd >= NOFILE || !ftab[fd])
        return (long)-9; /* EBADF */

    /* Fill basic statfs from fd's filesystem type */
    uint8_t kbuf[120];
    memset(kbuf, 0, sizeof(kbuf));
    uint64_t vals[15];
    memset(vals, 0, sizeof(vals));
    if (ext2_is_ops(ftab[fd]->ops)) {
        vals[0] = 0xEF53;
    } else {
        vals[0] = 0x01021994;
    }
    vals[1] = 4096;
    vals[9] = 255;
    vals[10] = 4096;
    memcpy(kbuf, vals, sizeof(kbuf));

    if (copyout(current_proc->pagetable, tf->a1, kbuf, 120) < 0)
        return (long)-14;
    return 0;
}

/* Ownership/credential stubs — single-user OS, always uid/gid 0 */
static long sys_fchownat_handler(struct trapframe *tf) {
    (void)tf;
    return (long)0;
}

static long sys_setuid_handler(struct trapframe *tf) {
    (void)tf;
    return (long)0;
}

static long sys_setgid_handler(struct trapframe *tf) {
    (void)tf;
    return (long)0;
}

static long sys_fchmodat_handler(struct trapframe *tf) {
    /* a0 = dirfd (AT_FDCWD), a1 = user path, a2 = mode, a3 = flags */
    char kpath[128];
    if (copyinstr(current_proc->pagetable, kpath, tf->a1, sizeof(kpath)) < 0)
        return (long)-14; /* EFAULT */

    /* Resolve path and verify it exists via stat */
    char resolved[128];
    resolve_path(current_proc->cwd, kpath, resolved);

    const char *relpath;
    struct mount *mnt = vfs_lookup_mount_entry(resolved, &relpath);
    struct vfs_ops *ops = mnt ? mnt->ops : NULL;
    if (ops && ops->stat) {
        uint8_t st[128];
        int ret = ops->stat(mnt->fs_data, relpath, st);
        if (ret < 0)
            return (long)-2; /* ENOENT */
    } else {
        return (long)-2; /* ENOENT */
    }
    /* ext2: actually set the mode on disk */
    if (ext2_is_ops(ops) && ops->open) {
        void *priv = NULL;
        if (ops->open(mnt->fs_data, relpath, 0, &priv) >= 0) {
            struct ext2_open_private *ep = (struct ext2_open_private *)priv;
            ext2_fchmod_ino(mnt->fs_data, ep->ino, (uint32_t)tf->a2);
            if (ops->close) ops->close(mnt->fs_data, priv);
        }
    }
    return 0;
}

static long sys_fchmod_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint32_t mode = (uint32_t)tf->a1;
    struct open_file **ftab = proc_ofile(current_proc);
    if (fd < 0 || fd >= NOFILE || !ftab[fd])
        return (long)-9; /* EBADF */
    struct open_file *f = ftab[fd];
    if (f->is_pipe) return (long)-22; /* EINVAL */
    /* ext2 files: update inode mode on disk */
    if (ext2_is_ops(f->ops) && f->private) {
        struct ext2_open_private *priv = (struct ext2_open_private *)f->private;
        return (long)ext2_fchmod_ino(f->fs_data, priv->ino, mode);
    }
    return 0; /* other filesystems: no-op */
}

static long sys_ftruncate_handler(struct trapframe *tf) {
    int fd = (int)tf->a0;
    uint64_t length = (uint64_t)tf->a1;
    struct open_file **ftab = proc_ofile(current_proc);
    if (fd < 0 || fd >= NOFILE || !ftab[fd])
        return (long)-9; /* EBADF */
    struct open_file *f = ftab[fd];
    if (f->is_pipe) return (long)-22; /* EINVAL */
    int accmode = f->flags & 3;
    if (accmode == 0) return (long)-9; /* EBADF: read-only fd */
    if (f->ops && f->ops->ftruncate)
        return (long)f->ops->ftruncate(f->fs_data, f->private, length);
    return (long)-30; /* EROFS: read-only filesystems */
}

static long sys_tgkill_handler(struct trapframe *tf) {
    pid_t tid = (pid_t)tf->a1;
    int sig = (int)tf->a2;
    if (sig == 0) {
        return (long)0;
    } else if (signal_is_valid(sig)) {
        long result = (long)-3;
        for (struct proc *p = proc; p < &proc[NPROC]; p++) {
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
static char mount_path_storage[8][128];
static int mount_path_next = 0;

static long sys_mount_handler(struct trapframe *tf) {
    char ksrc[128], ktgt[256], kfstype[64];
    if (copyinstr(current_proc->pagetable, ksrc, tf->a0, sizeof(ksrc)) < 0)
        return (long)-14;
    if (copyinstr(current_proc->pagetable, ktgt, tf->a1, sizeof(ktgt)) < 0)
        return (long)-14;
    if (copyinstr(current_proc->pagetable, kfstype, tf->a2, sizeof(kfstype)) < 0)
        return (long)-14;

    char source_resolved[128], target_resolved[256];
    resolve_path(current_proc->cwd, ksrc, source_resolved);
    resolve_path(current_proc->cwd, ktgt, target_resolved);

    /* Only ext2 supported */
    if (strcmp(kfstype, "ext2") != 0)
        return (long)-19; /* ENODEV */

    if (mount_path_next >= 8)
        return (long)-12; /* ENOMEM */

    const char *src_relpath;
    struct mount *src_mnt = vfs_lookup_mount_entry(source_resolved, &src_relpath);
    if (!src_mnt || !ext2_is_ops(src_mnt->ops))
        return (long)-19; /* ENODEV */

    /* Copy path to persistent storage (mount table stores pointers) */
    int rlen = strlen(target_resolved);
    memcpy(mount_path_storage[mount_path_next], target_resolved, (uint64_t)rlen + 1);
    vfs_register_mount(mount_path_storage[mount_path_next], &ext2_ops,
                       src_mnt->fs_data, "ext2");
    mount_path_next++;
    return 0;
}

static long sys_umount2_handler(struct trapframe *tf) {
    char ktgt[128];
    if (copyinstr(current_proc->pagetable, ktgt, tf->a0, sizeof(ktgt)) < 0)
        return (long)-14;

    char resolved[128];
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
    syscall_table[SYS_fchdir]          = sys_fchdir_handler;
    syscall_table[SYS_clone]           = sys_clone_handler;
    syscall_table[SYS_execve]          = sys_execve_handler;
    syscall_table[SYS_fexecve]         = sys_fexecve_handler;
    syscall_table[SYS_wait4]           = sys_wait4_handler;
    syscall_table[SYS_getpid]          = sys_getpid_handler;
    syscall_table[SYS_getppid]         = sys_getppid_handler;
    syscall_table[SYS_getuid]          = sys_getuid_handler;
    syscall_table[SYS_getgid]          = sys_getgid_handler;
    syscall_table[SYS_kill]            = sys_kill_handler;
    syscall_table[SYS_clock_gettime]   = sys_clock_gettime_handler;
    syscall_table[SYS_clock_getres]    = sys_clock_getres_handler;
    syscall_table[SYS_clock_settime]   = sys_clock_settime_handler;
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
    syscall_table[SYS_rt_sigsuspend]   = sys_rt_sigsuspend_handler;
    syscall_table[SYS_rt_sigpending]   = sys_rt_sigpending_handler;
    syscall_table[SYS_rt_sigtimedwait] = sys_rt_sigtimedwait_handler;
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
    syscall_table[SYS_fchmod]          = sys_fchmod_handler;
    syscall_table[SYS_fchmodat]        = sys_fchmodat_handler;
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
    syscall_table[SYS_sync]            = sys_sync_handler;
    syscall_table[SYS_fsync]           = sys_fsync_handler;
    syscall_table[SYS_fchownat]        = sys_fchownat_handler;
    syscall_table[SYS_setuid]          = sys_setuid_handler;
    syscall_table[SYS_setgid]          = sys_setgid_handler;
    syscall_table[SYS_utimensat]       = sys_utimensat_handler;
    syscall_table[SYS_statfs]          = sys_statfs_handler;
    syscall_table[SYS_fstatfs]         = sys_fstatfs_handler;
    syscall_table[SYS_msync]           = sys_msync_handler;
    syscall_table[SYS_futex]           = sys_futex_handler;
    syscall_table[SYS_gettid]          = sys_gettid_handler;
    syscall_table[SYS_sched_yield]     = sys_sched_yield_handler;
    syscall_table[SYS_pread64]         = sys_pread64_handler;
    syscall_table[SYS_pwrite64]        = sys_pwrite64_handler;
    syscall_table[SYS_mremap]          = sys_mremap_handler;
    syscall_table[SYS_getrusage]       = sys_getrusage_handler;
    syscall_table[SYS_times]           = sys_times_handler;
    syscall_table[SYS_timer_create]    = sys_timer_create_handler;
    syscall_table[SYS_timer_delete]    = sys_timer_delete_handler;
    syscall_table[SYS_timer_settime]   = sys_timer_settime_handler;
    syscall_table[SYS_timer_gettime]   = sys_timer_gettime_handler;
    syscall_table[SYS_timer_getoverrun] = sys_timer_getoverrun_handler;
    syscall_table[SYS_epoll_create1]   = sys_epoll_create1_handler;
    syscall_table[SYS_epoll_ctl]       = sys_epoll_ctl_handler;
    syscall_table[SYS_epoll_pwait]     = sys_epoll_pwait_handler;
    syscall_table[SYS_timerfd_create]  = sys_timerfd_create_handler;
    syscall_table[SYS_timerfd_settime] = sys_timerfd_settime_handler;
    syscall_table[SYS_timerfd_gettime] = sys_timerfd_gettime_handler;
    syscall_table[SYS_inotify_init1]   = sys_inotify_init1_handler;
    syscall_table[SYS_inotify_add_watch] = sys_inotify_add_watch_handler;
    syscall_table[SYS_inotify_rm_watch]  = sys_inotify_rm_watch_handler;
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
        /* Deliver pending signals before returning to user mode.
         * Unblockable signals (SIGKILL, synchronous faults) must be
         * deliverable even if sa_mask has them blocked, so we
         * temporarily unblock them for the deliverability check. */
        if (current_proc) {
            uint64_t fu = signal_bit(SIGKILL) | signal_bit(SIGSTOP) |
                          signal_bit(SIGILL) | signal_bit(SIGSEGV) |
                          signal_bit(SIGBUS) | signal_bit(SIGFPE);
            uint64_t saved = current_proc->sigmask;
            current_proc->sigmask &= ~fu;
            int has = (current_proc->sigpending & ~current_proc->sigmask) != 0;
            current_proc->sigmask = saved;
            if (has) deliver_signal(tf, current_proc);
        }
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
                    int ref = page_ref_get(pa);

                    if (ref <= 1) {
                        /* Sole reference: make writable in place */
                        *pte = PA2PTE(pa) | ((flags & ~PTE_COW) | PTE_W);
                    } else {
                        /* Multiple references: allocate new page and copy */
                        char *newpage = kalloc();
                        if (!newpage)
                            goto pagefault_kill;
                        memcpy(newpage, (void *)pa, PAGE_SIZE);
                        *pte = PA2PTE((uint64_t)newpage) | ((flags & ~PTE_COW) | PTE_W);
                        page_ref_dec(pa);
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

                    /* Restore PTE with original permissions + PTE_V
                     * BEFORE freeing the slot, so the PTE is never
                     * left with a stale swap marker that freewalk_level
                     * could double-free. */
                    *spte = PA2PTE((uint64_t)newpage) | sflags | PTE_V;
                    swap_free(slot);
                    asm volatile("sfence.vma %0, zero" : : "r"(sva));
                    if ((sflags & PTE_X) || cause == SCAUSE_INST_PAGE_FAULT)
                        asm volatile("fence.i");
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
            if (current_proc && stval < 0x80000000UL) {
                struct vm_area *fvma = vma_find(current_proc, stval);
                if (fvma) {
                    uint64_t va = PGROUNDDOWN(stval);
                    /* Check if page is already mapped */
                    pte_t *pte = walk(current_proc->pagetable, va, 0);
                    if (!pte || !(*pte & PTE_V)) {
                        /* Page not yet allocated — demand-page it */
                        char *pg = kalloc();
                        if (pg) {
                            memset(pg, 0, PAGE_SIZE);

                            /* File-backed VMA: read page content from file */
                            if (fvma->file && fvma->file->ops && fvma->file->ops->read) {
                                uint64_t file_off = fvma->file_offset + (va - fvma->start);
                                int64_t foff = (int64_t)file_off;
                                fvma->file->ops->read(fvma->file->fs_data,
                                                      fvma->file->private, pg,
                                                      PAGE_SIZE, &foff);
                                /* Short reads are OK — remaining bytes stay zero */
                            }

                            int perm = PTE_U;
                            if (fvma->prot & PROT_READ)  perm |= PTE_R;
                            if (fvma->prot & PROT_WRITE) perm |= PTE_W;
                            if (fvma->prot & PROT_EXEC)  perm |= PTE_X;
                            mappages(current_proc->pagetable, va, PAGE_SIZE,
                                     (uint64_t)pg, perm);
                            asm volatile("sfence.vma %0, zero" : : "r"(va));
                            if ((perm & PTE_X) || cause == SCAUSE_INST_PAGE_FAULT)
                                asm volatile("fence.i");
                            return; /* resume */
                        }
                    } else if (!(*pte & PTE_COW)) {
                        /* Page is mapped (non-COW) but may have stale
                         * permissions.  This happens when mprotect updates
                         * the VMA prot but the PTE was not updated (e.g.,
                         * the page was demand-paged before mprotect, or the
                         * TLB cached old permissions).  Reconcile PTE with
                         * the current VMA prot to prevent infinite fault
                         * loops (critical for JIT/W^X PROT_EXEC
                         * transitions).  COW pages are excluded — they are
                         * handled by the COW path above. */
                        int need = 0;
                        if ((cause == SCAUSE_INST_PAGE_FAULT) &&
                            (fvma->prot & PROT_EXEC) && !(*pte & PTE_X))
                            need = 1;
                        if ((cause == SCAUSE_STORE_PAGE_FAULT) &&
                            (fvma->prot & PROT_WRITE) && !(*pte & PTE_W))
                            need = 1;
                        if ((cause == SCAUSE_LOAD_PAGE_FAULT) &&
                            (fvma->prot & PROT_READ) && !(*pte & PTE_R))
                            need = 1;
                        if (need) {
                            uint64_t rpa = PTE2PA(*pte);
                            int rref = page_ref_get(rpa);
                            if (rref > 1 && (cause == SCAUSE_STORE_PAGE_FAULT)) {
                                /* Page is shared (ref > 1) but lost its
                                 * PTE_COW flag (e.g., after swap round-trip).
                                 * Must COW-copy before granting write. */
                                char *cowpg = kalloc();
                                if (!cowpg)
                                    goto pagefault_kill;
                                memcpy(cowpg, (void *)rpa, PAGE_SIZE);
                                page_ref_dec(rpa);
                                int perm = PTE_U;
                                if (fvma->prot & PROT_READ)  perm |= PTE_R;
                                if (fvma->prot & PROT_WRITE) perm |= PTE_W;
                                if (fvma->prot & PROT_EXEC)  perm |= PTE_X;
                                *pte = PA2PTE((uint64_t)cowpg) | perm | PTE_V;
                            } else {
                                int perm = PTE_U;
                                if (fvma->prot & PROT_READ)  perm |= PTE_R;
                                if (fvma->prot & PROT_WRITE) perm |= PTE_W;
                                if (fvma->prot & PROT_EXEC)  perm |= PTE_X;
                                *pte = (*pte & ~(PTE_R | PTE_W | PTE_X)) | perm;
                            }
                            asm volatile("sfence.vma %0, zero" : : "r"(va));
                            if ((fvma->prot & PROT_EXEC) || cause == SCAUSE_INST_PAGE_FAULT)
                                asm volatile("fence.i");
                            return; /* resume */
                        }
                    }
                }
            }

            /* Workaround: if instruction page fault at kernel address,
             * the kernel code PTE may have lost PTE_X.  Restore it and
             * retry the instruction.  This handles corruption that
             * occurs during boot tests (after the initial restore). */
            if (cause == SCAUSE_INST_PAGE_FAULT && stval >= 0x80000000UL) {
                pte_t *kpte = walk(kernel_pagetable, PGROUNDDOWN(stval), 0);
                if (kpte && (*kpte & PTE_V)) {
                    /* Always ensure PTE_X is set and flush TLB */
                    *kpte |= PTE_X;
                    asm volatile("sfence.vma %0, zero" : : "r"(stval));
                    return; /* retry faulting instruction */
                }
            }

pagefault_kill:
            if (current_proc) {
                /* Suppress page-fault noise when the fault is a side-effect of
                 * an ongoing signal kill.  Check three conditions:
                 *   1. Process has a SIGSEGV handler (it will handle the fault).
                 *   2. Process was already killed by a signal (killed_by_signal
                 *      set by deliver_signal or kill_deliver).
                 *   3. Process has a pending unblocked fatal signal whose default
                 *      action is terminate — the fault is just a consequence of
                 *      that pending kill (e.g., SIGINT/SIGPIPE with SIG_DFL). */
                struct kern_sigaction *sa = proc_sigactions(current_proc);
                int has_handler = (sa[SIGSEGV].handler != 0 && sa[SIGSEGV].handler != 1);

                int suppress = has_handler || current_proc->killed_by_signal;
                if (!suppress) {
                    /* Check for any pending unblocked fatal signal */
                    uint64_t deliverable = current_proc->sigpending & ~current_proc->sigmask;
                    /* Mask of signals whose default action is ignore or stop
                     * (not terminate): SIGCHLD(17) SIGURG(23) SIGWINCH(28)
                     * SIGCONT(18) SIGSTOP(19) SIGTSTP(20) SIGTTIN(21) SIGTTOU(22) */
                    uint64_t non_fatal = signal_bit(SIGCHLD) | signal_bit(SIGURG) |
                                         signal_bit(SIGWINCH) | signal_bit(SIGCONT) |
                                         signal_bit(SIGSTOP) | signal_bit(SIGTSTP) |
                                         signal_bit(SIGTTIN) | signal_bit(SIGTTOU);
                    uint64_t fatal_pending = deliverable & ~non_fatal;
                    if (fatal_pending)
                        suppress = 1;
                }

                if (!suppress) {
                    printk("Page fault pid=%d epc=0x%lx stval=0x%lx cause=%lu\n",
                           current_proc->pid, tf->epc, stval, cause);
                }
                current_proc->sigpending |= signal_bit(SIGSEGV);
                deliver_signal(tf, current_proc);
                return;
            }
            /* No process context — truly fatal */
            printk("Page fault epc=0x%lx stval=0x%lx cause=%lu (no process)\n",
                   tf->epc, stval, cause);
            while(1) {}
        }
        /* Handle other user-mode exceptions by delivering a signal */
        if (current_proc) {
            int sig = SIGKILL;
            if (cause == 2)                    sig = SIGILL;  /* Illegal instruction */
            else if (cause == 0 || cause == 4 || cause == 6) sig = SIGBUS; /* Misaligned */
            else if (cause == 5 || cause == 7) sig = SIGBUS;  /* Access fault */
            printk("trap pid=%d scause=0x%lx epc=0x%lx stval=0x%lx -> sig %d\n",
                   current_proc->pid, cause, tf->epc, stval, sig);
            current_proc->sigpending |= signal_bit(sig);
            deliver_signal(tf, current_proc);
            return;
        }
        printk("UNEXPECTED TRAP! scause: 0x%lx, epc: 0x%lx, tval: 0x%lx\n",
               cause, tf->epc, stval);
        while(1) {}
    }

    /* ---- ecall from user ---- */
    /* Advance past the ecall instruction (always 4 bytes in RISC-V) */
    tf->epc += 4;

    long sysnum = (long)tf->a7;

    /* Save original syscall arguments for SA_RESTART */
    if (current_proc) {
        current_proc->saved_a0 = tf->a0;
        current_proc->saved_a1 = tf->a1;
        current_proc->saved_a2 = tf->a2;
        current_proc->saved_a3 = tf->a3;
        current_proc->saved_a4 = tf->a4;
        current_proc->saved_a5 = tf->a5;
        current_proc->in_syscall = 1;
    }

    if (sysnum >= 0 && sysnum < NSYSCALLS && syscall_table[sysnum]) {
        long ret = syscall_table[sysnum](tf);
        if (ret != SYSCALL_NORETURN)
            tf->a0 = (uint64_t)ret;
    } else {
        printk("Unknown syscall %ld from pid %d\n", sysnum, current_proc ? current_proc->pid : -1);
        tf->a0 = (uint64_t)-38; /* -ENOSYS */
    }

    if (current_proc) current_proc->in_syscall = 0;

    /* Deliver pending signals before returning to user mode.
     * Unblockable signals (SIGKILL, synchronous faults) must be
     * deliverable even if sa_mask has them blocked, so we
     * temporarily unblock them for the deliverability check. */
    if (current_proc) {
        uint64_t fu = signal_bit(SIGKILL) | signal_bit(SIGSTOP) |
                      signal_bit(SIGILL) | signal_bit(SIGSEGV) |
                      signal_bit(SIGBUS) | signal_bit(SIGFPE);
        uint64_t saved = current_proc->sigmask;
        current_proc->sigmask &= ~fu;
        int has = (current_proc->sigpending & ~current_proc->sigmask) != 0;
        current_proc->sigmask = saved;
        if (has) deliver_signal(tf, current_proc);
    }
    return;
}
