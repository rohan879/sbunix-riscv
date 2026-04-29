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
#include <mm.h>
#include <rtc.h>
/* RF: New headers for trap.c modular decomposition */
#include <syscall_handlers.h>
#include <trap_internal.h>
#include <pipe.h>

/* RF: Made non-static for access by extracted syscall modules via trap_internal.h */
unsigned long ticks;

/* Get current tick count - accessible from other modules */
unsigned long get_ticks(void) {
  return ticks;
}
unsigned char last_key = 0;

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
    struct open_file *f = srctab[i];
    if (src->fd_flags[i] & FD_CLOFORK) {
      /* ENH: Skip O_CLOFORK file descriptors during fork */
      dst->ofile[i] = 0;
      dst->fd_flags[i] = 0;
      continue;
    }
    if (f && !vfs_file_valid(f)) {
      srctab[i] = 0;
      src->fd_flags[i] = 0;
      dst->ofile[i] = 0;
      dst->fd_flags[i] = 0;
      continue;
    }
    dst->ofile[i] = f;
    /* Copy fd_flags for non-skipped fds */
    dst->fd_flags[i] = src->fd_flags[i];
    if (f) {
      f->refcount++;
      /* bump pipe refcounts too */
      if (f->is_pipe) {
        int pid = f->pipe_id;
        if (f->pipe_read && pid >= 0)
          pipes[pid].read_open++;
        if (f->pipe_write && pid >= 0)
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
  if (!vfs_file_valid(f))
    return;

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

  /* BF: Use the shared VFS drop path so file-backed VMAs and fd closes
   * have identical last-reference teardown semantics. */
  vfs_close_file(f);
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
    uint64_t boot_ticks = r_time();
    uint64_t epoch_ns = 0;
    int realtime_valid = 0;

    ticks = 0;
    rtc_init();

    if (rtc_available()) {
        epoch_ns = rtc_read_epoch_ns();
        uint64_t epoch_sec = epoch_ns / NSEC_PER_SEC;
        if (epoch_sec >= 1577836800ULL && epoch_sec < 4102444800ULL)
            realtime_valid = 1;
    }

    timekeeping_init(boot_ticks, epoch_ns, realtime_valid);
    sbi_set_timer(r_time() + TIMER_FREQ);
}

/* Return seconds elapsed since boot (for /proc/uptime) */
unsigned long get_uptime_secs(void) {
    uint64_t elapsed = timekeeping_elapsed_ticks();
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
    unsigned long elapsed = timekeeping_elapsed_ticks() / TIMER_FREQ;
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

    /* Fast per-tick check of critical kernel code PTE.  The repair path
     * enforces W^X instead of blindly restoring execute permission. */
    recursive_check_kcode_pte();
    /* Full scan every 100 ticks (~10s at 10Hz) */
    if (ticks % 100 == 50) {
      int repairs = recursive_check_all_kernel_ptes();
      if (repairs > 0)
        printk("timer: repaired %d kernel PTE(s)\n", repairs);
    }
    
    /* BF: Reclaim decisions depend on reasonably fresh page aging data; long
     * intervals made memory-pressure tests run with stale eviction signals. */
    if (ticks % 10 == 0) {
        extern struct page *page_table;
        if (page_table) {
            mm_age_pages();
        }
    }
    
    /* OPT: Read hardware timer once and reuse for all deadline checks
     * below (timed-sleep, SIGALRM, POSIX timers) to avoid repeated
     * CSR reads within the same interrupt handler invocation. */
    uint64_t now = r_time();

    /* Wake processes whose timed sleep has expired */
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        if (p->state == SLEEPING && p->waiting_for_uart == 2 &&
            now >= p->sleep_until) {
            p->waiting_for_uart = 0;
            p->state = RUNNABLE;
        }
    }
    /* Check for expired alarms and deliver SIGALRM */
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
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
        /* OPT: Skip the NPTIMER inner loop entirely for processes with
         * no active POSIX timers.  Safe on single-hart: SIE is cleared
         * on trap entry so active_timer_count cannot change mid-loop. */
        if (p->active_timer_count == 0)
            continue;
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
                        /* OPT: Decrement active count when overrun-disarm fires */
                        if (p->active_timer_count > 0)
                            p->active_timer_count--;
                    }
                } else {
                    /* One-shot: disarm timer */
                    p->timers[i].next_expiry_ticks = 0;
                    if (p->active_timer_count > 0)
                        p->active_timer_count--;
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

static void signal_default_terminate(struct proc *p, int signum) {
    p->exit_status = 128 + signum;
    p->killed_by_signal = signum;
    p->stopped_signal = 0;
    cleanup_proc_resources(p);
    p->state = ZOMBIE;
    release_vfork_parent(p);
    if (p->parent) {
        sig_set_pending_ex(p->parent, SIGCHLD,
                           (int32_t)p->pid, 0, K_CLD_KILLED,
                           p->killed_by_signal);
        if (p->parent->state == SLEEPING)
            p->parent->state = RUNNABLE;
        struct kern_sigaction *psa = proc_sigactions(p->parent);
        if (psa[SIGCHLD].flags & 0x00000002) { /* SA_NOCLDWAIT */
            struct proc *initproc = 0;
            for (struct proc *q = proc; q < &proc[NPROC]; q++) {
                if (q->pid == 1 && q->state != UNUSED) {
                    initproc = q;
                    break;
                }
            }
            p->parent = initproc;
            if (initproc && initproc->state == SLEEPING)
                initproc->state = RUNNABLE;
        }
    }
    swtch(&p->context, &scheduler_context);
}

static void signal_frame_fault(struct proc *p) {
    if (p->has_saved_sigmask) {
        p->sigmask = p->saved_sigmask_restore;
        p->has_saved_sigmask = 0;
    }
    p->sigmask &= ~signal_bit(SIGSEGV);
    p->sigpending &= ~signal_bit(SIGSEGV);
    struct kern_sigaction *sa = proc_sigactions(p);
    sa[SIGSEGV].handler = 0;
    sa[SIGSEGV].flags = 0;
    signal_default_terminate(p, SIGSEGV);
}

struct user_ucontext {
    uint64_t uc_link;
    uint64_t uc_sigmask;
    struct {
        uint64_t ss_sp;
        int32_t ss_flags;
        int32_t pad;
        uint64_t ss_size;
    } uc_stack;
    uint64_t regs[32];
};

static void fill_user_ucontext(struct user_ucontext *uc,
                               const struct trapframe *tf,
                               struct proc *p,
                               uint64_t saved_sigmask) {
    memset(uc, 0, sizeof(*uc));
    uc->uc_link = 0;
    uc->uc_sigmask = saved_sigmask;
    uc->uc_stack.ss_sp = p->ss_sp;
    if (p->ss_sp == 0 || p->ss_size == 0)
        uc->uc_stack.ss_flags = 2; /* SS_DISABLE */
    else
        uc->uc_stack.ss_flags = p->ss_flags;
    uc->uc_stack.ss_size = p->ss_size;

    uc->regs[0] = 0;
    uc->regs[1] = tf->ra;
    uc->regs[2] = tf->sp;
    uc->regs[3] = tf->gp;
    uc->regs[4] = tf->tp;
    uc->regs[5] = tf->t0;
    uc->regs[6] = tf->t1;
    uc->regs[7] = tf->t2;
    uc->regs[8] = tf->s0;
    uc->regs[9] = tf->s1;
    uc->regs[10] = tf->a0;
    uc->regs[11] = tf->a1;
    uc->regs[12] = tf->a2;
    uc->regs[13] = tf->a3;
    uc->regs[14] = tf->a4;
    uc->regs[15] = tf->a5;
    uc->regs[16] = tf->a6;
    uc->regs[17] = tf->a7;
    uc->regs[18] = tf->s2;
    uc->regs[19] = tf->s3;
    uc->regs[20] = tf->s4;
    uc->regs[21] = tf->s5;
    uc->regs[22] = tf->s6;
    uc->regs[23] = tf->s7;
    uc->regs[24] = tf->s8;
    uc->regs[25] = tf->s9;
    uc->regs[26] = tf->s10;
    uc->regs[27] = tf->s11;
    uc->regs[28] = tf->t3;
    uc->regs[29] = tf->t4;
    uc->regs[30] = tf->t5;
    uc->regs[31] = tf->t6;
}

static int signal_queue_has_more(struct proc *p, int signum) {
    for (int i = 0; i < NSIGQUEUE; i++) {
        if (p->sigqueue_buf[i].valid && p->sigqueue_buf[i].signum == signum)
            return 1;
    }
    return 0;
}

static int signal_queue_consume_one(struct proc *p, int signum, uint8_t *info_out) {
    for (int i = 0; i < NSIGQUEUE; i++) {
        if (p->sigqueue_buf[i].valid && p->sigqueue_buf[i].signum == signum) {
            if (info_out)
                memcpy(info_out, p->sigqueue_buf[i].info, 128);
            p->sigqueue_buf[i].valid = 0;
            return 1;
        }
    }
    return 0;
}

static void signal_queue_purge_all(struct proc *p, int signum) {
    for (int i = 0; i < NSIGQUEUE; i++) {
        if (p->sigqueue_buf[i].valid && p->sigqueue_buf[i].signum == signum)
            p->sigqueue_buf[i].valid = 0;
    }
}

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
    for (int i = 1; i < 65; i++) {
        if (deliverable & signal_bit(i)) {
            signum = i;
            break;
        }
    }

    struct kern_sigaction *sa = proc_sigactions(p);
    
    /* Force SIG_DFL for SIG_IGN on unblockable/synchronous signals */
    if (sa[signum].handler == 1 && (force_unblock & signal_bit(signum))) {
        sa[signum].handler = 0;
    }
    
    uint64_t handler = sa[signum].handler;
    uint64_t handler_mask = sa[signum].mask;
    int handler_flags = sa[signum].flags;

    if (handler >= 2 && (handler < 0x1000 || !user_addr_ok(handler))) {
        handler = 0;
    }

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
            p->sigpending &= ~signal_bit(signum);
            signal_queue_purge_all(p, signum);
            CONSUME_DEFERRED_MASK(p);
            return;
        }

        /* Continue: resume stopped process (or ignore if running) */
        if (signum == SIGCONT) {
            p->sigpending &= ~signal_bit(signum);
            signal_queue_purge_all(p, signum);
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
            p->sigpending &= ~signal_bit(signum);
            signal_queue_purge_all(p, signum);
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

        p->sigpending &= ~signal_bit(signum);
        signal_queue_purge_all(p, signum);
        CONSUME_DEFERRED_MASK(p);
        signal_default_terminate(p, signum);
        return;
    }

    /* SIG_IGN (1) -- ignore */
    if (handler == 1) {
        p->sigpending &= ~signal_bit(signum);
        signal_queue_purge_all(p, signum);
        CONSUME_DEFERRED_MASK(p);
        return;
    }

    /* User handler: save trapframe on user stack, redirect execution */
    uint64_t sp = tf->sp;

    /* Use alternate signal stack if SA_ONSTACK and alt stack is configured */
    if ((handler_flags & 0x08000000) && /* SA_ONSTACK */
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
    uint64_t ucontext_uaddr = 0;
    if (handler_flags & 0x00000004) { /* SA_SIGINFO */
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
        int found_queued = signal_queue_consume_one(p, signum, ksi);
        if (found_queued) {
            p->sig_sender[signum].valid = 0;
            if (signal_queue_has_more(p, signum))
                p->sigpending |= signal_bit(signum);
            /* BF: Force si_signo to match delivered signal (POSIX requires this) */
            int32_t si_signo = (int32_t)signum;
            memcpy(ksi + 0, &si_signo, 4);
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
        if (copyout_proc(p, sp, ksi, 128) < 0) {
            signal_frame_fault(p);
            return;
        }

        uint64_t frame_sigmask =
            p->has_saved_sigmask ? p->saved_sigmask_restore : p->sigmask;
        struct user_ucontext uctx;
        fill_user_ucontext(&uctx, tf, p, frame_sigmask);
        sp -= sizeof(uctx);
        sp &= ~15UL;
        ucontext_uaddr = sp;
        if (copyout_proc(p, sp, &uctx, sizeof(uctx)) < 0) {
            signal_frame_fault(p);
            return;
        }
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
    if ((handler_flags & 0x10000000) &&  /* SA_RESTART */
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

    if (copyout_proc(p, sp, &frame, sizeof(frame)) < 0) {
        signal_frame_fault(p);
        return;
    }

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
            signal_frame_fault(p);
            return;
        }
        tf->ra = SIGTRAMP_VA;
    } else {
        tf->ra = (uint64_t)sa[signum].restorer;
    }

    /* Set up trapframe to call handler(signum[, siginfo, ucontext]) */
    if (handler >= 2) {
        /* The handler frame is ready; consume this signal now. */
        p->sigpending &= ~signal_bit(signum);
        if ((handler_flags & 0x00000004) && signal_queue_has_more(p, signum))
            p->sigpending |= signal_bit(signum);
        if (!(handler_flags & 0x00000004))
            signal_queue_purge_all(p, signum);
        /* BF: Bump epoch so ppoll/pselect6 can detect that a signal was
         * handled between their mask swap and the blocking sleep. */
        p->signal_handled_epoch++;
        tf->epc = handler;                   /* jump to signal handler */
        tf->a0 = (uint64_t)signum;           /* first arg: signal number */
        if (handler_flags & 0x00000004) { /* SA_SIGINFO */
            tf->a1 = siginfo_uaddr;         /* second arg: siginfo_t * */
            tf->a2 = ucontext_uaddr;        /* third arg: ucontext_t * */
        }
        tf->sp = sp;                         /* stack points to saved trapframe */
        /* tf->ra already set above: SIGTRAMP_VA or sa[signum].restorer */

        /* Block this signal during handler execution */
        p->sigmask |= handler_mask;
        /* BF: SA_RESETHAND implies SA_NODEFER per POSIX -- when the handler
         * resets to SIG_DFL after one invocation, the signal should not be
         * blocked during that invocation. */
        if (!(handler_flags & 0x40000000) &&      /* SA_NODEFER */
            !(handler_flags & (int)0x80000000))    /* SA_RESETHAND */
            p->sigmask |= signal_bit(signum);

        /* SA_RESETHAND: reset handler to SIG_DFL after delivery */
        if (handler_flags & (int)0x80000000) { /* SA_RESETHAND */
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
        release_vfork_parent(target);
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
    syscall_table[SYS_waitid]          = sys_waitid_handler;
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
    syscall_table[SYS_prctl]           = sys_prctl_handler;
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
    unsigned long sstatus = r_sstatus();
    int from_user = (sstatus & SSTATUS_SPP) == 0;

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
        if (current_proc && from_user) {
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
            if (from_user && cause == SCAUSE_STORE_PAGE_FAULT && current_proc) {
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

            /* --- Stack auto-grow (any page fault near current stack) --- */
            if (from_user && current_proc && current_proc->stack_top > 0 &&
                stval >= current_proc->stack_bottom - 256 * PAGE_SIZE &&
                stval < current_proc->stack_top) {
                uint64_t va = PGROUNDDOWN(stval);
                int grew = 0;
                while (current_proc->stack_bottom > va) {
                    current_proc->stack_bottom -= PAGE_SIZE;
                    char *pg = kalloc();
                    if (!pg)
                        break;
                    memset(pg, 0, PAGE_SIZE);
                    if (mappages(current_proc->pagetable, current_proc->stack_bottom,
                                 PAGE_SIZE, (uint64_t)pg, PTE_R | PTE_W | PTE_U) < 0) {
                        kfree(pg);
                        break;
                    }
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
            if (from_user && current_proc && user_addr_ok(stval)) {
                struct vm_area *fvma = vma_find(current_proc, stval);
                    if (fvma) {
                    /* BF: Enforce VMA protection flags -- deliver SIGSEGV if the fault
                     * type doesn't match the VMA's prot bits.  POSIX requires SIGSEGV
                     * for writes to PROT_READ-only, and any access to PROT_NONE. */
                    if (fvma->prot == 0 /* PROT_NONE */) {
                        /* PROT_NONE: no access allowed at all */
                        goto pagefault_kill;
                    }
                    if (cause == SCAUSE_INST_PAGE_FAULT && !(fvma->prot & PROT_EXEC)) {
                        /* Execute fault on non-executable mapping */
                        goto pagefault_kill;
                    }
                    if (cause == SCAUSE_LOAD_PAGE_FAULT && !(fvma->prot & PROT_READ)) {
                        /* Read fault on non-readable mapping */
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

                            int perm = PTE_U | prot_to_pte_flags(fvma->prot);
                            if (mappages(current_proc->pagetable, va, PAGE_SIZE,
                                         (uint64_t)pg, perm) != 0) {
                                /* BF: A failed PTE install leaves the new
                                 * frame unattached; free it before killing
                                 * the process to avoid retry leaks under
                                 * allocation pressure. */
                                kfree(pg);
                                goto pagefault_kill;
                            }
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
                                 * PTE_COW flag. Must COW-copy before
                                 * granting write. */
                                char *cowpg = kalloc();
                                if (!cowpg)
                                    goto pagefault_kill;
                                memcpy(cowpg, (void *)rpa, PAGE_SIZE);
                                int perm = PTE_U | prot_to_pte_flags(fvma->prot);
                                if (pte_update(current_proc->pagetable, va, pte,
                                               PA2PTE((uint64_t)cowpg) | perm | PTE_V,
                                               0, PTE_UPDATE_COW,
                                               PTE_SFENCE_NONE) < 0) {
                                    kfree(cowpg);
                                    goto pagefault_kill;
                                }
                                if (fvma->anon_vma) {
                                    struct page *cow_page = virt_to_page(cowpg);
                                    if (cow_page)
                                        page_add_anon_rmap(cow_page, fvma, va);
                                }
                                page_ref_dec(rpa);
                            } else {
                                int perm = PTE_U | prot_to_pte_flags(fvma->prot);
                                if (pte_update(current_proc->pagetable, va, pte,
                                               (*pte & ~(PTE_R | PTE_W | PTE_X)) | perm,
                                               0, PTE_UPDATE_MPROTECT,
                                               PTE_SFENCE_NONE) < 0)
                                    goto pagefault_kill;
                            }
                            asm volatile("sfence.vma %0, zero" : : "r"(va));
                            if ((fvma->prot & PROT_EXEC) || cause == SCAUSE_INST_PAGE_FAULT)
                                asm volatile("fence.i");
                            return; /* resume */
                        }
                    }
                }
            }

            if (cause == SCAUSE_INST_PAGE_FAULT && stval >= KERNBASE) {
                if (recursive_repair_kernel_pte(PGROUNDDOWN(stval)))
                    return;
                if (!kernel_va_executable(stval))
                    goto pagefault_kill;
                /* If we reach here, the executable kernel PTE may already have
                 * been repaired between the CPU fault and our handler entry.
                 * Retry the faulting instruction, but limit retries to avoid
                 * infinite loops from persistent corruption. */
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
            if (!from_user) {
                printk("Kernel page fault epc=0x%lx stval=0x%lx cause=%lu sstatus=0x%lx\n",
                       tf->epc, stval, cause, sstatus);
                while(1) {}
            }
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
        if (current_proc && from_user) {
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
        printk("UNEXPECTED TRAP! scause: 0x%lx, epc: 0x%lx, tval: 0x%lx sstatus=0x%lx\n",
               cause, tf->epc, stval, sstatus);
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
