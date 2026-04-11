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
/* RF: New headers for trap.c modular decomposition */
#include <syscall_handlers.h>
#include <trap_internal.h>
#include <pipe.h>
#include <pathres.h>

/* Goldfish RTC at 0x101000 (QEMU virt machine) */
#define GOLDFISH_RTC_BASE 0x101000UL
#define GOLDFISH_RTC_TIME_LOW  0x00
#define GOLDFISH_RTC_TIME_HIGH 0x04

/* RF: Made non-static for access by extracted syscall modules via trap_internal.h */
unsigned long boot_time;
unsigned long ticks;

/* Get current tick count - accessible from other modules */
unsigned long get_ticks(void) {
  return ticks;
}
/* RF: Made non-static for access by extracted syscall modules via trap_internal.h */
uint64_t boot_epoch_ns;  /* RTC nanoseconds at boot */
unsigned char last_key = 0;

/* RF: Made non-static for access by extracted time syscall module */
uint64_t ns_to_time_ticks(uint64_t ns) {
    return (ns + 99) / 100;
}

/* RF: Made non-static for access by extracted time syscall module */
uint64_t timespec_rel_deadline(uint64_t sec, uint64_t nsec) {
    uint64_t delta = sec * TIMER_FREQ + ns_to_time_ticks(nsec);
    return r_time() + delta;
}

static void rtc_init(void) {
    volatile uint32_t *rtc = (volatile uint32_t *)GOLDFISH_RTC_BASE;
    uint32_t lo = rtc[0];  /* TIME_LOW */
    uint32_t hi = rtc[1];  /* TIME_HIGH */
    boot_epoch_ns = ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* Get current wall-clock time as {sec, nsec}
 * RF: Made non-static for access by extracted time syscall module */
void get_wall_time(int64_t *sec, int64_t *nsec) {
    uint64_t elapsed_ticks = r_time() - boot_time;
    uint64_t elapsed_ns = elapsed_ticks * 100; /* 10MHz -> ns */
    uint64_t now_ns = boot_epoch_ns + elapsed_ns;
    *sec  = (int64_t)(now_ns / 1000000000ULL);
    *nsec = (int64_t)(now_ns % 1000000000ULL);
}

/* RF: Kernel pipe table (pipes[], alloc_pipe, pipe_avail) moved to syscall_ipc.c.
 * Still accessible here via pipe.h extern declarations. */

/* ------------------------------------------------------------------ */
/* File descriptor helpers                                              */
/* ------------------------------------------------------------------ */

/* Allocate a free fd slot in current_proc; returns fd or -1
 * RF: Made non-static for access by extracted syscall modules */
int alloc_fd(void) {
  struct open_file **ftable = proc_ofile(current_proc);
  for (int i = 0; i < NOFILE; i++) {
    if (ftable[i] == NULL)
      return i;
  }
  return -1;
}

/* Shallow copy fd table from src to dst (for clone), incrementing refcounts
 * RF: Made non-static for access by extracted process syscall module */
void copy_ofile(struct proc *dst, struct proc *src) {
  struct open_file **srctab = proc_ofile(src);
  /* ENH: FD_CLOFORK (bit 2) causes the fd to be skipped during fork,
   * per POSIX O_CLOFORK semantics.  Non-skipped fds inherit fd_flags. */
  #define FD_CLOFORK 2
  for (int i = 0; i < NOFILE; i++) {
    if (src->fd_flags[i] & FD_CLOFORK) {
      /* ENH: Skip O_CLOFORK file descriptors during fork */
      dst->ofile[i] = 0;
      dst->fd_flags[i] = 0;
      continue;
    }
    dst->ofile[i] = srctab[i];
    /* Copy fd_flags for non-skipped fds */
    dst->fd_flags[i] = src->fd_flags[i];
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

/* ------------------------------------------------------------------ */
/* File lock support (POSIX and OFD record locks for fcntl)            */
/* ------------------------------------------------------------------ */

/* RF: lock_table, ranges_overlap, lock_conflicts, do_file_setlk,
 * do_file_getlk, file_locks_release_pid, file_locks_release_ofd
 * moved to syscall_io.c */

/* Close an open_file: decrement refcount, handle pipe cleanup, free when zero.
 * Also clears the slot in the given proc's ofile[fd] (or shared_ft).
 * RF: Made non-static for access by extracted syscall modules */
void close_ofile(struct proc *p, int fd) {
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
    /* Release any OFD locks held by this open_file before freeing it.
     * POSIX: OFD locks are tied to the open file description, so they
     * must be released when the last fd referencing it is closed. */
    file_locks_release_ofd(f);
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

    /* BF: Track per-process CPU time for CLOCK_PROCESS_CPUTIME_ID.
     * Each timer tick (~100ms at 10Hz) while a process is running
     * increments that process's cputime_ticks counter. */
    if (current_proc)
        current_proc->cputime_ticks++;

    /* Fast per-tick check of critical kernel code PTE via self-referencing
     * gigapage window (single volatile load + conditional store).
     * Catches DMA-induced PTE_X corruption before it causes faults. */
    recursive_check_kcode_pte();
    /* Full scan every 100 ticks (~10s at 10Hz) */
    if (ticks % 100 == 50) {
      int repairs = recursive_check_all_kernel_ptes();
      if (repairs > 0)
        printk("timer: repaired %d kernel PTE(s)\n", repairs);
    }
    
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
/* RF: Made non-static for access by extracted process syscall module */
int copy_pagetable_cow(pagetable_t dst, pagetable_t src,
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

        int orig_flags = PTE_FLAGS(*pte);  /* BF: save before CoW conversion for rollback */

        if (flags & PTE_W) {
            /* Writable page: mark read-only + COW in both parent and child */
            *pte = PA2PTE(pa) | ((flags & ~PTE_W) | PTE_COW | PTE_V);
            flags = (flags & ~PTE_W) | PTE_COW;
        }

        /* Share the same physical page */
        page_ref_inc(pa);
        if (mappages(dst, va, PAGE_SIZE, pa, flags) < 0) {
            /* BF: Rollback — restore parent PTE and decrement refcount
             * leaked by the failed child mapping attempt. */
            *pte = PA2PTE(pa) | orig_flags;
            page_ref_dec(pa);
            sfence_vma();
            return -1;
        }

        /* BF: B4 (page_add_mapping) intentionally omitted here.
         * copy_pagetable_cow runs for ALL pages (anon + non-anon), but
         * freewalk_level in the exit path does kfree without a matching
         * page_remove_mapping.  This causes mapcount to stay elevated
         * after the child exits, making pages permanently unevictable
         * by swap_evict_pages (which skips mapcount > 0).  The VMA-level
         * rmap from anon_vma_fork is sufficient for swap/reclaim. */
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
/* RF: Made non-static for access by extracted process syscall module */
void cleanup_proc_resources(struct proc *p) {
    /* ---- POSIX file locks ---- */
    /* POSIX: all locks held by a process are released on exit.
     * OFD locks are released separately when the open_file refcount hits 0. */
    file_locks_release_pid(p->pid);

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
    /* ENH: Clear per-fd flags on process exit */
    memset(p->fd_flags, 0, sizeof(p->fd_flags));

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
/* RF: Made non-static for access by extracted process syscall module */
pid_t sys_clone(struct trapframe *caller_tf) {
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
    /* BF: Propagate UID/GID to child so kill() permission checks
     * remain consistent after fork. */
    p->uid = current_proc->uid;
    p->gid = current_proc->gid;
    p->clone_flags = clone_flags;

    /* BF: POSIX requires fork to inherit the parent's alternate signal
     * stack settings.  Without this, allocproc's default SS_DISABLE
     * overwrites the parent's configured alt stack, causing sigaltstack
     * GET in the child to report SS_DISABLE instead of the inherited
     * flags. */
    p->ss_sp = current_proc->ss_sp;
    p->ss_size = current_proc->ss_size;
    p->ss_flags = current_proc->ss_flags;

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
    /* BF: Fork child inherits parent's kstack_npages for proper stack top */
    p->context.sp = (uint64_t)p->kstack + (uint64_t)p->kstack_npages * PAGE_SIZE;

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
/* RF: Made non-static for access by extracted process syscall module */
pid_t sys_wait4(pid_t wpid, int *wstatus, int options) {
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
                /* BF: Free all pages of the kernel stack (1 or 2 pages) */
                if (p->kstack) {
                    kfree_contiguous(p->kstack, p->kstack_npages);
                    p->kstack = NULL;
                }
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
            /* BF: Mark this process as a wait4 sleeper so setpgid can
             * selectively wake only waitpid callers, not pipe/poll/futex. */
            current_proc->waiting_for_child = 1;
            current_proc->state = SLEEPING;
            swtch(&current_proc->context, &scheduler_context);
            /* BF: Clear wait4 sleeper flag after waking — process is no
             * longer blocked in wait4. */
            current_proc->waiting_for_child = 0;
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

/* RF: Path resolution helpers moved to kernel/pathres.c */
#include <tar.h>
extern char _tarfs_start[];
extern char _tarfs_end[];

/* ------------------------------------------------------------------ */
/* Signal delivery                                                      */
/* ------------------------------------------------------------------ */

/* RF: Made non-static for access by extracted signal syscall module */
void deliver_signal(struct trapframe *tf, struct proc *p) {
    /* BF: Only SIGKILL and SIGSTOP are truly unblockable per POSIX.
     * Synchronous fault signals (SIGILL, SIGSEGV, SIGBUS, SIGFPE) are
     * unmasked at their exception handler sites, not here. */
    uint64_t force_unblock = signal_bit(SIGKILL) | signal_bit(SIGSTOP);

    /* Find lowest-numbered pending unblocked signal.  Unblockable signals
     * must be deliverable even if sa_mask has them blocked. */
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
            /* BF: Notify parent with CLD_CONTINUED so SA_SIGINFO handlers
             * on SIGCHLD can detect child continuation. */
            if (p->parent) {
                struct kern_sigaction *psa = proc_sigactions(p->parent);
                if (!(psa[SIGCHLD].flags & 0x00000001)) { /* SA_NOCLDSTOP */
                    /* BF: Don't overwrite sigchld_code if SIGCHLD is already
                     * pending -- a prior child's exit/stop code would be lost. */
                    if (!(p->parent->sigpending & signal_bit(SIGCHLD)))
                        p->parent->sigchld_code = 6; /* CLD_CONTINUED */
                    p->parent->sigpending |= signal_bit(SIGCHLD);
                    if (p->parent->state == SLEEPING)
                        p->parent->state = RUNNABLE;
                }
            }
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
                    /* BF: Don't overwrite sigchld_code if SIGCHLD is already
                     * pending -- a prior child's exit/stop code would be lost. */
                    if (!(p->parent->sigpending & signal_bit(SIGCHLD)))
                        p->parent->sigchld_code = 5; /* CLD_STOPPED */
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
            /* BF: Don't overwrite sigchld_code if SIGCHLD is already pending */
            if (!(p->parent->sigpending & signal_bit(SIGCHLD)))
                p->parent->sigchld_code = 2; /* CLD_KILLED */
            p->parent->sigpending |= signal_bit(SIGCHLD);
            if (p->parent->state == SLEEPING)
                p->parent->state = RUNNABLE;
            /* BF: SA_NOCLDWAIT on parent means auto-reap -- reparent to
             * init so waitpid in parent doesn't block on this zombie.
             * Mirrors the same logic in sys_exit_handler. */
            struct kern_sigaction *psa = proc_sigactions(p->parent);
            if (psa[SIGCHLD].flags & 0x00000002) { /* SA_NOCLDWAIT */
                struct proc *initproc = 0;
                for (struct proc *q = proc; q < &proc[NPROC]; q++) {
                    if (q->pid == 1 && q->state != UNUSED) { initproc = q; break; }
                }
                p->parent = initproc;
                if (initproc && initproc->state == SLEEPING)
                    initproc->state = RUNNABLE;
            }
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
        /* BF: Populate all required siginfo_t fields at their correct offsets.
         * siginfo_t layout: { int si_signo(0), si_errno(4), si_code(8);
         *   pid_t si_pid(12); uid_t si_uid(16); int si_status(20); ... }
         * Previously only si_signo and si_code were set, and si_pid was at
         * the wrong offset (16 instead of 12). */
        uint8_t ksi[128];
        memset(ksi, 0, sizeof(ksi));
        int32_t si_signo = (int32_t)signum;
        memcpy(ksi + 0, &si_signo, 4);   /* offset 0: si_signo */
        int32_t si_errno = 0;
        memcpy(ksi + 4, &si_errno, 4);   /* offset 4: si_errno = 0 */
        /* BF: Use process-specific sigchld_code for SIGCHLD so SA_SIGINFO
         * handlers can distinguish CLD_STOPPED/CLD_CONTINUED/CLD_EXITED. */
        int32_t si_code = 0; /* SI_USER for kill-delivered signals */
        if (signum == SIGCHLD && p->sigchld_code != 0) {
            si_code = p->sigchld_code;
            p->sigchld_code = 0; /* consumed */
        }
        memcpy(ksi + 8, &si_code, 4);    /* offset 8: si_code */
        int32_t si_pid = current_proc ? (int32_t)current_proc->pid : 0;
        memcpy(ksi + 12, &si_pid, 4);    /* offset 12: si_pid (sender) */
        int32_t si_uid = 0;
        memcpy(ksi + 16, &si_uid, 4);    /* offset 16: si_uid = 0 (single-user OS) */

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
    /* BF: If ppoll/pselect6 deferred a mask restore (so the signal can be
     * delivered under the temporary unblocked mask), save the ORIGINAL mask
     * into the frame so sigreturn restores it after the handler completes. */
    if (p->has_saved_sigmask) {
        frame.saved_sigmask = p->saved_sigmask_restore;
        p->has_saved_sigmask = 0;
    } else {
        frame.saved_sigmask = p->sigmask;
    }
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
        /* BF: Bump epoch so ppoll/pselect6 can detect that a signal was
         * handled between their mask swap and the blocking sleep. */
        p->signal_handled_epoch++;
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
        /* BF: SA_RESETHAND implies SA_NODEFER per POSIX -- when the handler
         * resets to SIG_DFL after one invocation, the signal should not be
         * blocked during that invocation. */
        if (!(sa[signum].flags & 0x40000000) &&      /* SA_NODEFER */
            !(sa[signum].flags & (int)0x80000000))    /* SA_RESETHAND */
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

/* RF: SYSCALL_NORETURN, NSYSCALLS now defined in syscall_handlers.h.
 * syscall_table made non-static for access by extracted modules. */
long (*syscall_table[NSYSCALLS])(struct trapframe *tf);

/* ---- Individual syscall handler functions ---- */

/* RF: sys_brk_handler moved to syscall_memory.c */

/* RF: sys_write_handler moved to syscall_io.c */

/* RF: sys_read_handler moved to syscall_io.c */

/* RF: sys_openat_handler, sys_close_handler, sys_dup3_handler moved to syscall_io.c */

/* RF: sys_pipe2_handler moved to syscall_ipc.c */

/* RF: sys_getcwd_handler, sys_chdir_handler, sys_fchdir_handler moved to syscall_io.c */

long sys_clone_handler(struct trapframe *tf) {
    pid_t child = sys_clone(tf);
    tf->a0 = (uint64_t)child;
    return SYSCALL_NORETURN;
}

long sys_execve_handler(struct trapframe *tf) {
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

    /* POSIX: close file descriptors marked with FD_CLOEXEC.
     * ENH: Check per-fd fd_flags (bit 0 = FD_CLOEXEC) in addition to the
     * legacy O_CLOEXEC check on the open file flags, so that both old and
     * new paths are covered. */
    {
        #define KERN_O_CLOEXEC 02000000 /* matches libc O_CLOEXEC */
        struct open_file **oft = proc_ofile(current_proc);
        for (int fd = 0; fd < NOFILE; fd++) {
            if (oft[fd] && ((oft[fd]->flags & KERN_O_CLOEXEC) ||
                            (current_proc->fd_flags[fd] & 1))) {
                close_ofile(current_proc, fd);
                current_proc->fd_flags[fd] = 0;
            }
        }
    }

    /* BF: POSIX requires caught signal dispositions (user handlers) to be
     * reset to SIG_DFL on exec.  SIG_IGN dispositions are preserved.
     * Also reset the alternate signal stack to SS_DISABLE since the old
     * stack memory is gone after exec. */
    {
        struct kern_sigaction *sa = proc_sigactions(current_proc);
        for (int i = 1; i < 32; i++) {
            if (sa[i].handler > 1) { /* user handler, not SIG_DFL(0) or SIG_IGN(1) */
                sa[i].handler = 0;  /* reset to SIG_DFL */
                sa[i].flags = 0;
                sa[i].mask = 0;
            }
        }
    }
    current_proc->ss_sp = 0;
    current_proc->ss_size = 0;
    current_proc->ss_flags = 2; /* SS_DISABLE */

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

long sys_fexecve_handler(struct trapframe *tf) {
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

long sys_wait4_handler(struct trapframe *tf) {
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

long sys_getpid_handler(struct trapframe *tf) {
    (void)tf;
    return current_proc ? (long)current_proc->pid : 0;
}

long sys_getppid_handler(struct trapframe *tf) {
    (void)tf;
    if (current_proc && current_proc->parent)
        return (long)current_proc->parent->pid;
    else
        return (long)1;
}

long sys_getuid_handler(struct trapframe *tf) {
    (void)tf;
    /* BF: Return actual process UID instead of hardcoded 0 so that
     * userspace identity checks (e.g. kill permission) are accurate. */
    return (long)current_proc->uid;
}

long sys_getgid_handler(struct trapframe *tf) {
    (void)tf;
    /* BF: Return actual process GID instead of hardcoded 0. */
    return (long)current_proc->gid;
}

/* Helper: deliver signal to a single process */
/* RF: Made non-static for access by extracted signal syscall module */
void kill_deliver(struct proc *target, int sig) {
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

/* RF: sys_kill_handler, sys_rt_sigqueueinfo_handler
 * moved to syscall_signal.c */

/* RF: sys_clock_gettime_handler, sys_clock_getres_handler, sys_clock_settime_handler
 * moved to syscall_time.c */

/* RF: sys_lseek_handler, sys_fstat_handler, sys_newfstatat_handler,
 * sys_getdents64_handler moved to syscall_io.c */

/* RF: sys_nanosleep_handler moved to syscall_time.c */

long sys_exit_handler(struct trapframe *tf) {
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
            /* BF: Set sigchld_code so SA_SIGINFO SIGCHLD handlers get the
             * correct si_code (CLD_EXITED for normal exit, CLD_KILLED if
             * terminated by a signal). */
            /* BF: Don't overwrite sigchld_code if SIGCHLD is already
             * pending -- a prior child's exit/stop code would be lost. */
            if (!(par->sigpending & signal_bit(SIGCHLD)))
                par->sigchld_code = current_proc->killed_by_signal ? 2 : 1;
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

/* RF: sys_fsync_handler, sys_sync_handler, sys_reboot_handler moved to syscall_misc.c */

/* RF: mmap_writeback_shared, mmap_search_base, mmap_find_hole moved to syscall_memory.c */

/* RF: sys_mmap_handler, mmap_writeback_shared moved to syscall_memory.c */

/* RF: sys_munmap_handler moved to syscall_memory.c */

/* RF: sys_mprotect_handler moved to syscall_memory.c */

long sys_ps_handler(struct trapframe *tf) {
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

/* RF: sys_ioctl_handler moved to syscall_misc.c */

/* RF: sys_rt_sigaction_handler, sys_rt_sigprocmask_handler,
 * sys_rt_sigreturn_handler, sys_rt_sigsuspend_handler,
 * sys_rt_sigpending_handler, sys_rt_sigtimedwait_handler
 * moved to syscall_signal.c */

/* RF: sys_gettimeofday_handler moved to syscall_time.c */

/* RF: sys_fcntl_handler, sys_faccessat_handler, sys_unlinkat_handler,
 * sys_linkat_handler, sys_renameat2_handler, sys_mkdirat_handler,
 * sys_mknodat_handler, sys_readlinkat_handler, sys_symlinkat_handler
 * moved to syscall_io.c */

/* RF: sys_umask_handler, sys_geteuid_handler, sys_getegid_handler,
 * sys_set_tid_address_handler moved to syscall_misc.c */

/* RF: sys_writev_handler, sys_readv_handler moved to syscall_io.c */

/* RF: sys_prlimit64_handler, sys_getrandom_handler moved to syscall_misc.c */

long sys_setpgid_handler(struct trapframe *tf) {
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
    /* BF: Wake all wait4 sleepers — they need to re-evaluate match
     * criteria after pgid change.  The previous parent-only wake was
     * too narrow (non-parent waitpid(-pgid) callers missed the wake).
     * A blanket wake was too broad (hit pipe/poll sleepers).  The
     * waiting_for_child flag precisely targets wait4 sleepers. */
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        if (p->state == SLEEPING && p->waiting_for_child)
            p->state = RUNNABLE;
    }
    return 0;
}

long sys_setsid_handler(struct trapframe *tf) {
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

long sys_getpgid_handler(struct trapframe *tf) {
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

long sys_getsid_handler(struct trapframe *tf) {
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

/* RF: sys_alarm_handler, sys_timer_create_handler, sys_timer_delete_handler,
 * sys_timer_settime_handler, sys_timer_gettime_handler,
 * sys_timer_getoverrun_handler moved to syscall_time.c */

/* RF: sys_madvise_handler moved to syscall_memory.c */

/* RF: sys_futex_handler and FUTEX_* defines moved to syscall_ipc.c */

/* RF: sys_msync_handler moved to syscall_memory.c */

/* RF: sys_gettid_handler, sys_sched_yield_handler moved to syscall_misc.c */

/* RF: sys_pread64_handler, sys_pwrite64_handler moved to syscall_io.c */

/* RF: sys_mremap_handler moved to syscall_memory.c */

/* RF: sys_getrusage_handler, sys_times_handler moved to syscall_time.c */

/* RF: epoll, timerfd, inotify structs and handlers moved to syscall_ipc.c */

/* RF: ramfs_inode_trap, ramfs_private structs moved to syscall_io.c and syscall_misc.c */

/* RF: sys_utimensat_handler, sys_statfs_handler, sys_fstatfs_handler,
 * sys_fchownat_handler, sys_setuid_handler, sys_setgid_handler
 * moved to syscall_misc.c */

/* RF: sys_fchmodat_handler, sys_fchmod_handler, sys_ftruncate_handler
 * moved to syscall_io.c */

/* RF: sys_tgkill_handler moved to syscall_signal.c */

/* Poll event constants (must match libc/include/poll.h) */
/* RF: KPOLL* constants now defined in pipe.h */

/* RF: check_fd_ready, sys_ppoll_handler, sys_pselect6_handler moved to syscall_ipc.c */

/* RF: sys_clock_nanosleep_handler moved to syscall_time.c */

/* RF: sys_sigaltstack_handler moved to syscall_signal.c */

/* RF: sys_mount_handler, sys_umount2_handler, sys_uname_handler
 * moved to syscall_misc.c */

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
    syscall_table[SYS_rt_sigqueueinfo] = sys_rt_sigqueueinfo_handler;
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
        /* BF: Narrowed force_unblock to only SIGKILL/SIGSTOP (same as
         * syscall return path) -- synchronous signals unmasked at fault. */
        if (current_proc) {
            uint64_t fu = signal_bit(SIGKILL) | signal_bit(SIGSTOP);
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
                /* BF: Guard — reject kernel-only PTEs in COW handler */
                if (pte && (*pte & PTE_V) && !(*pte & PTE_U))
                    goto pagefault_kill;

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
                    /* BF: Register swapped-in page in anon rmap so the
                     * reclaim subsystem can find and evict it again. */
                    {
                      struct vm_area *swap_vma = vma_find(current_proc, sva);
                      if (swap_vma && swap_vma->anon_vma) {
                        struct page *sp = virt_to_page(newpage);
                        if (sp) page_add_anon_rmap(sp, swap_vma, sva);
                      }
                    }
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
                    /* BF: Register stack page in anon rmap so reclaim can
                     * track and evict stack pages under memory pressure. */
                    {
                      struct vm_area *stk_vma = vma_find(current_proc, current_proc->stack_bottom);
                      if (stk_vma && stk_vma->anon_vma) {
                        struct page *stk_pg = virt_to_page(pg);
                        if (stk_pg) page_add_anon_rmap(stk_pg, stk_vma, current_proc->stack_bottom);
                      }
                    }
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
                    /* BF: Enforce VMA protection flags -- deliver SIGSEGV if the fault
                     * type doesn't match the VMA's prot bits.  POSIX requires SIGSEGV
                     * for writes to PROT_READ-only, and any access to PROT_NONE. */
                    if (fvma->prot == 0 /* PROT_NONE */) {
                        /* PROT_NONE: no access allowed at all */
                        goto pagefault_kill;
                    }
                    if (cause == SCAUSE_STORE_PAGE_FAULT && !(fvma->prot & PROT_WRITE)) {
                        /* Write to non-writable mapping */
                        goto pagefault_kill;
                    }

                    uint64_t va = PGROUNDDOWN(stval);
                    /* BF: Guard — reject kernel-only superpages before demand-paging.
                     * These are shared MMIO entries from uvmmap_kernel() that should
                     * not be demand-paged by user processes. */
                    {
                        int pte_level = -1;
                        pte_t *spte = walk_super(current_proc->pagetable, va, &pte_level);
                        if (spte && (*spte & PTE_V) && !(*spte & PTE_U) && pte_level > 0)
                            goto pagefault_kill;
                    }
                    /* Original walk() for demand-paging logic — unchanged */
                    pte_t *pte = walk(current_proc->pagetable, va, 0);
                    if (!pte || !(*pte & PTE_V)) {
                        /* BF: POSIX requires SIGBUS for file-backed mmap access
                         * beyond the end of the file (rounded up to page boundary).
                         * Check file size via fstat before demand-paging. */
                        if (fvma->file && fvma->file->ops && fvma->file->ops->fstat) {
                            uint8_t stbuf[128];
                            memset(stbuf, 0, sizeof(stbuf));
                            if (fvma->file->ops->fstat(fvma->file->fs_data,
                                                       fvma->file->private, stbuf) == 0) {
                                int64_t file_sz = *(int64_t *)(stbuf + 48); /* st_size */
                                uint64_t file_end_page = ((uint64_t)file_sz + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
                                uint64_t page_file_off = fvma->file_offset + (va - fvma->start);
                                if (page_file_off >= file_end_page) {
                                    current_proc->sigpending |= signal_bit(SIGBUS);
                                    deliver_signal(tf, current_proc);
                                    return;
                                }
                            }
                        }

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
                            /* BF: Register demand-faulted anonymous page in rmap
                             * so reclaim can track and evict it later. */
                            if ((fvma->flags & MAP_ANON) && fvma->anon_vma) {
                              struct page *dp = virt_to_page(pg);
                              if (dp) page_add_anon_rmap(dp, fvma, va);
                            }
                            return; /* resume */
                        }
                    } else if (!(*pte & PTE_COW)) {
                        /* BF: Guard — skip kernel-only PTEs in stale-permissions path */
                        if (!(*pte & PTE_U))
                            goto pagefault_kill;
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

            /* Use self-referencing gigapage window to repair PTE without
             * walk() and without depending on the identity-mapped superpages
             * (which are the PTEs being corrupted by DMA). */
            if (cause == SCAUSE_INST_PAGE_FAULT && stval >= 0x80000000UL) {
                if (kernel_pagetable[RECURSIVE_INDEX] & PTE_V) {
                    volatile pte_t *ppte = recursive_l1_pte(PGROUNDDOWN(stval));
                    if (ppte) {
                        pte_t pte = *ppte;
                        if ((pte & PTE_V) && !(pte & PTE_X)) {
                            *ppte = pte | PTE_X;
                            asm volatile("sfence.vma zero, zero" ::: "memory");
                            asm volatile("fence.i" ::: "memory");
                            return;
                        }
                        if (!(pte & PTE_V)) {
                            uint64_t pa = SPGROUNDDOWN(stval);
                            *ppte = PA2PTE(pa) | PTE_V | PTE_R | PTE_W | PTE_X;
                            asm volatile("sfence.vma zero, zero" ::: "memory");
                            asm volatile("fence.i" ::: "memory");
                            return;
                        }
                    }
                }
                /* Fallback: direct identity-map access (works if identity
                 * map is not corrupted) */
                {
                    pte_t l2pte = kernel_pagetable[2];
                    if (l2pte & PTE_V) {
                        pagetable_t l1 = (pagetable_t)PTE2PA(l2pte);
                        int idx = PX(1, stval);
                        if ((l1[idx] & PTE_V) && !(l1[idx] & PTE_X)) {
                            l1[idx] |= PTE_X;
                            asm volatile("sfence.vma zero, zero" ::: "memory");
                            asm volatile("fence.i" ::: "memory");
                            return;
                        }
                    }
                }
                /* BF: If we reach here, the PTE already has PTE_X -- the timer
                 * interrupt may have repaired it between the CPU fault and our
                 * handler entry.  Retry the faulting instruction, but limit
                 * retries to avoid infinite loops from persistent corruption. */
                {
                    static int kpte_retry_count;
                    static uint64_t kpte_retry_epc;
                    if (tf->epc == kpte_retry_epc) {
                        kpte_retry_count++;
                    } else {
                        kpte_retry_epc = tf->epc;
                        kpte_retry_count = 1;
                    }
                    if (kpte_retry_count <= 3) {
                        /* Full TLB flush + instruction cache sync -- the
                         * targeted sfence.vma may not suffice if the stale
                         * translation is cached in the I-TLB. */
                        asm volatile("sfence.vma zero, zero" ::: "memory");
                        asm volatile("fence.i" ::: "memory");
                        return;
                    }
                    /* Too many retries at same EPC -- fall through to kill */
                    kpte_retry_count = 0;
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

    /* BF: in_syscall must remain set through deliver_signal so the
     * SA_RESTART check (which tests in_syscall && a0==-EINTR) fires
     * correctly.  Cleared AFTER delivery. */

    /* Deliver pending signals before returning to user mode.
     * BF: Narrowed force_unblock to only truly unblockable signals
     * (SIGKILL, SIGSTOP).  Synchronous fault signals (SIGILL, SIGSEGV,
     * SIGBUS, SIGFPE) are unmasked at their exception handler sites,
     * not here -- force-unblocking them here breaks SA_SIGINFO tests
     * that install handlers for these signals. */
    if (current_proc) {
        uint64_t fu = signal_bit(SIGKILL) | signal_bit(SIGSTOP);
        uint64_t saved = current_proc->sigmask;
        current_proc->sigmask &= ~fu;
        int has = (current_proc->sigpending & ~current_proc->sigmask) != 0;
        current_proc->sigmask = saved;
        if (has) deliver_signal(tf, current_proc);
        current_proc->in_syscall = 0;
    }
    return;
}
