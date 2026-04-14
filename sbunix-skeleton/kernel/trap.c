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
#include <tty.h>
#include <swap.h>
#include <mm.h>
/* RF: New headers for trap.c modular decomposition */
#include <syscall_handlers.h>
#include <trap_internal.h>
#include <pipe.h>

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
    
    /* OPT: Age pages every 100 ticks (~10s) instead of every 10 ticks (~1s)
     * to reduce per-tick overhead; page aging is expensive and infrequent
     * aging is sufficient for the swap eviction heuristic. */
    if (ticks % 100 == 0) {
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
            /* BF: Capture timer metadata for SA_SIGINFO delivery */
            sig_set_pending(p, 14, 0, 0, K_SI_TIMER);
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
                /* BF: Capture timer metadata for SA_SIGINFO delivery */
                sig_set_pending(p, p->timers[i].signo, 0, 0, K_SI_TIMER);
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

/* RF: copy_pagetable_cow, cleanup_proc_resources, sys_clone, sys_wait4
 * moved to syscall_process.c */

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
    if (deliverable == 0) {
        /* BF: Clear has_saved_sigmask when no signal is actually delivered.
         * Without this, a deferred mask restore from ppoll/pselect6 would
         * persist across unrelated syscalls, corrupting the process sigmask
         * on a later sigreturn. */
        p->has_saved_sigmask = 0;
        return;
    }

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

    /* BF: Helper macro — consume deferred sigsuspend mask on any path
     * that does NOT build a user signal frame (where sigreturn would
     * normally restore it).  Without this, sigsuspend returns with the
     * temporary mask still installed, violating POSIX. */
    #define CONSUME_DEFERRED_MASK(proc) do { \
        if ((proc)->has_saved_sigmask) { \
            (proc)->sigmask = (proc)->saved_sigmask_restore; \
            (proc)->has_saved_sigmask = 0; \
        } \
    } while(0)

    /* SIG_DFL (0) -- default action */
    if (handler == 0) {
        /* Ignore-class signals */
        if (signum == SIGCHLD || signum == SIGURG || signum == SIGWINCH) {
            CONSUME_DEFERRED_MASK(p);
            return;
        }

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
                    /* BF: Capture child metadata for parent's SIGCHLD handler.
                     * sig_set_pending_ex checks pending internally. */
                    sig_set_pending_ex(p->parent, SIGCHLD,
                                       (int32_t)p->pid, 0, K_CLD_CONTINUED, 0);
                    if (p->parent->state == SLEEPING)
                        p->parent->state = RUNNABLE;
                }
            }
            CONSUME_DEFERRED_MASK(p);
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
                    /* BF: Capture child metadata for parent's SIGCHLD handler.
                     * deliver_signal path, distinct from kill_deliver Fix 2+3c. */
                    sig_set_pending_ex(p->parent, SIGCHLD,
                                       (int32_t)p->pid, 0, K_CLD_STOPPED, signum);
                    if (p->parent->state == SLEEPING)
                        p->parent->state = RUNNABLE;
                }
            }
            CONSUME_DEFERRED_MASK(p);
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
            /* BF: Capture child metadata for parent's SIGCHLD handler */
            sig_set_pending_ex(p->parent, SIGCHLD,
                               (int32_t)p->pid, 0, K_CLD_KILLED,
                               p->killed_by_signal);
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
    if (handler == 1) {
        CONSUME_DEFERRED_MASK(p);
        return;
    }

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
        /* BF: Search sigqueue_buf for queued siginfo from sigqueue().
         * FIFO ordering is approximate (first-valid-match scan, not
         * sequence-ordered).  Acceptable for a teaching OS since repeated
         * same-signal queueing is rare. */
        int found_queued = 0;
        for (int qi = 0; qi < NSIGQUEUE; qi++) {
            if (p->sigqueue_buf[qi].valid &&
                p->sigqueue_buf[qi].signum == signum) {
                memcpy(ksi, p->sigqueue_buf[qi].info, 128);
                p->sigqueue_buf[qi].valid = 0;
                found_queued = 1;
                /* BF: Force si_signo to match delivered signal (POSIX requires this) */
                int32_t si_signo = (int32_t)signum;
                memcpy(ksi + 0, &si_signo, 4);
                break;
            }
        }
        if (!found_queued) {
            /* BF: Use sig_sender metadata captured at generation time so
             * si_pid/si_code reflect the sender, not the target at delivery. */
            int32_t si_signo = (int32_t)signum;
            memcpy(ksi + 0, &si_signo, 4);   /* offset 0: si_signo */
            int32_t si_errno = 0;
            memcpy(ksi + 4, &si_errno, 4);   /* offset 4: si_errno = 0 */
            int32_t si_code = 0, si_pid = 0, si_uid = 0, si_status = 0;
            if (p->sig_sender[signum].valid) {
                si_code = p->sig_sender[signum].si_code;
                si_pid = p->sig_sender[signum].si_pid;
                si_uid = p->sig_sender[signum].si_uid;
                si_status = p->sig_sender[signum].si_status;
                p->sig_sender[signum].valid = 0;
            }
            memcpy(ksi + 8, &si_code, 4);    /* offset 8: si_code */
            memcpy(ksi + 12, &si_pid, 4);    /* offset 12: si_pid (sender) */
            memcpy(ksi + 16, &si_uid, 4);    /* offset 16: si_uid */
            memcpy(ksi + 20, &si_status, 4); /* offset 20: si_status (SIGCHLD) */
        }

        /* BF: Allocate 128 bytes on user stack for siginfo (matches ksi[] buffer
         * size).  Larger than K_SIGINFO_SIZE but must match the copyout below.
         * Do NOT use K_SIGINFO_SIZE here — deliver_signal controls both the
         * allocation and the copyout, so 128 is correct and intentional. */
        sp -= 128;
        sp &= ~15UL;
        siginfo_uaddr = sp;
        /* BF: Demand-page siginfo target (same lazy-alloc issue as signal frame) */
        {
            pte_t *pte = walk(p->pagetable, sp & ~(uint64_t)(PAGE_SIZE - 1), 0);
            if (!pte || !(*pte & PTE_V)) {
                char *mem = kalloc();
                if (mem) {
                    memset(mem, 0, PAGE_SIZE);
                    mappages(p->pagetable, sp & ~(uint64_t)(PAGE_SIZE - 1),
                             PAGE_SIZE, (uint64_t)mem, PTE_R | PTE_W | PTE_U);
                }
            }
        }
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

    /* BF: Ensure signal frame target pages are mapped before copyout.
     * Alt stack memory allocated via malloc/mmap may be lazily mapped —
     * no page fault fires because the user never touched the memory before
     * the kernel tries to write the signal frame.  Walk the page table for
     * each page spanned by the frame and demand-allocate if unmapped. */
    {
        uint64_t frame_end = sp + sizeof(frame);
        for (uint64_t pg = (sp & ~(uint64_t)(PAGE_SIZE - 1));
             pg < frame_end; pg += PAGE_SIZE) {
            pte_t *pte = walk(p->pagetable, pg, 0);
            if (!pte || !(*pte & PTE_V)) {
                char *mem = kalloc();
                if (mem) {
                    memset(mem, 0, PAGE_SIZE);
                    mappages(p->pagetable, pg, PAGE_SIZE,
                             (uint64_t)mem, PTE_R | PTE_W | PTE_U);
                }
            }
        }
    }

    /* Copy signal frame to user stack via copyout */
    if (copyout(p->pagetable, sp, &frame, sizeof(frame)) < 0)
        return; /* can't deliver signal -- just drop it */

    /* BF: Use per-process sigtrampoline page mapped at SIGTRAMP_VA when
     * no sa_restorer is set.  External binaries may not set restorer —
     * the trampoline executes rt_sigreturn to return from the handler.
     * Previously, missing restorer killed the process unconditionally. */
    if (sa[signum].restorer == 0) {
        /* BF: Guard against processes that never called exec (e.g. PID 1
         * before init_starter runs) — SIGTRAMP_VA would be unmapped.
         * walk() returns NULL if the page is not in the page table. */
        pte_t *pte = walk(p->pagetable, SIGTRAMP_VA, 0);
        if (!pte || !(*pte & PTE_V)) {
            return; /* can't deliver — drop signal rather than fault */
        }
        tf->ra = SIGTRAMP_VA;
    } else {
        tf->ra = (uint64_t)sa[signum].restorer;
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
        /* tf->ra already set above: SIGTRAMP_VA or sa[signum].restorer */

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

/* RF: All sys_*_handler functions have been extracted to their
 * respective kernel/syscall_*.c modules.  See syscall_handlers.h
 * for the complete list of handler prototypes and their locations. */

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
            /* BF: Capture child metadata for parent's SIGCHLD handler */
            sig_set_pending_ex(target->parent, SIGCHLD,
                               (int32_t)target->pid, 0, K_CLD_KILLED, 9);
            if (target->parent->state == SLEEPING)
                target->parent->state = RUNNABLE;
        }
        /* If killing ourselves, swtch away immediately — we're ZOMBIE
         * and must not continue executing on this kernel stack. */
        if (target == current_proc)
            swtch(&target->context, &scheduler_context);
    } else if (signal_is_valid(sig)) {
        /* BF: Capture sender metadata at generation time for SA_SIGINFO */
        sig_set_pending(target, sig,
                        current_proc ? (int32_t)current_proc->pid : 0,
                        0, K_SI_USER);
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
            target->exit_status = (sig << 8) | 0x7f; /* BF: Fix 2 — for WIFSTOPPED/WSTOPSIG */
            target->state = SLEEPING;
            target->sigpending &= ~signal_bit(SIGCONT);
            /* Notify parent (unless SA_NOCLDSTOP) */
            if (target->parent) {
                struct kern_sigaction *psa = proc_sigactions(target->parent);
                if (!(psa[SIGCHLD].flags & 0x00000001)) { /* SA_NOCLDSTOP */
                    /* BF: Fix 3c — capture child metadata for parent's SIGCHLD handler */
                    sig_set_pending_ex(target->parent, SIGCHLD,
                                       (int32_t)target->pid, 0, K_CLD_STOPPED,
                                       sig /* si_status = stop signal */);
                    if (target->parent->state == SLEEPING)
                        target->parent->state = RUNNABLE;
                }
            }
        } else if (target->state == SLEEPING) {
            target->state = RUNNABLE;
        }
    }
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
            /* BF: Refactored to use cow_break() which also registers the
             * new private page in anon rmap for the eviction scanner. */
            if (cause == SCAUSE_STORE_PAGE_FAULT && current_proc) {
                uint64_t va = PGROUNDDOWN(stval);
                pte_t *pte = walk(current_proc->pagetable, va, 0);
                /* BF: Guard — reject kernel-only PTEs in COW handler */
                if (pte && (*pte & PTE_V) && !(*pte & PTE_U))
                    goto pagefault_kill;

                if (pte && (*pte & PTE_V) && (*pte & PTE_COW)) {
                    struct vm_area *vma = vma_find(current_proc, va);
                    if (cow_break(current_proc->pagetable, pte, va, vma) < 0)
                        goto pagefault_kill;
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
                                    /* BF: Capture sender metadata for SA_SIGINFO */
                                    sig_set_pending(current_proc, SIGBUS,
                                                    (int32_t)current_proc->pid, 0, 0);
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
                /* BF: Capture sender metadata for SA_SIGINFO */
                sig_set_pending(current_proc, SIGSEGV,
                                (int32_t)current_proc->pid, 0, 0);
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
            /* BF: Capture sender metadata for SA_SIGINFO */
            sig_set_pending(current_proc, sig,
                            (int32_t)current_proc->pid, 0, 0);
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
        /* BF: Clear stale has_saved_sigmask at syscall entry.  Without this,
         * a flag left over from a previous ppoll/pselect6 that returned
         * normally (not via signal) would leak into the next syscall and
         * cause deliver_signal to restore the wrong sigmask. */
        current_proc->has_saved_sigmask = 0;
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
