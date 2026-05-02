/* RF: Signal-related syscall handlers extracted from trap.c.
 * kill, rt_sig*, tgkill, sigaltstack.
 * Note: deliver_signal() and kill_deliver() remain in trap.c
 * (called by trap_handler directly). */

#include <syscall_handlers.h>
#include <trap_internal.h>
#include <proc.h>
#include <riscv.h>
#include <string.h>
#include <printk.h>
#include <vm.h>
#include <errno.h>

static int rt_sigsetsize_valid(uint64_t sigsetsize) {
    return sigsetsize == sizeof(uint64_t);
}

static int signal_send_allowed(struct proc *target) {
    if (!current_proc || !target)
        return 0;
    return current_proc->uid == 0 || current_proc->uid == target->uid;
}

static struct proc *find_signal_target(pid_t pid) {
    /* BF: A process signaling itself should never fail with ESRCH.
     * During signal-heavy tests the currently running proc can be the
     * only authoritative handle even if host-visible proc table state
     * is in flux around wake/reap transitions. */
    if (current_proc && current_proc->pid == pid &&
        current_proc->state != UNUSED && current_proc->state != ZOMBIE)
        return current_proc;
    for (int i = 0; i < NPROC; i++) {
        if (proc[i].state != UNUSED && proc[i].pid == pid)
            return &proc[i];
    }
    return NULL;
}

static void restore_user_signal_context(struct trapframe *dst,
                                        const struct trapframe *src) {
    uint64_t safe_sstatus = dst->saved_sstatus;
    safe_sstatus &= ~SSTATUS_SPP;
    safe_sstatus |= SSTATUS_SPIE;

    memcpy(dst, src, __builtin_offsetof(struct trapframe, saved_sstatus));
    dst->saved_sstatus = safe_sstatus;
    dst->return_to_user = 1;
}

long sys_kill_handler(struct trapframe *tf) {
    pid_t target_pid = (pid_t)(int)tf->a0;
    int sig = (int)tf->a1;

    if (sig < 0)
        return (long)-22; /* -EINVAL */
    if (sig != 0 && !signal_is_valid(sig))
        return (long)-22; /* -EINVAL */

    if (target_pid == 0) {
        /* Send to all processes in caller's process group */
        pid_t pgid = current_proc->pgid;
        int found = 0;
        int eperm = 0;
        for (int i = 0; i < NPROC; i++) {
            if (proc[i].state != UNUSED && proc[i].state != ZOMBIE &&
                proc[i].pgid == pgid) {
                if (!signal_send_allowed(&proc[i])) {
                    eperm = 1;
                    continue;
                }
                found = 1;
                /* BF: sig=0 is existence/permission check only -- no delivery.
                 * Prevents undefined behavior from signal_bit(0) = 1 << -1. */
                if (sig != 0)
                    kill_deliver(&proc[i], sig);
            }
        }
        if (found)
            return 0;
        return eperm ? (long)-1 : (long)-3; /* EPERM or ESRCH */
    }

    if (target_pid < -1) {
        /* Send to all processes in process group abs(target_pid) */
        pid_t pgid = -target_pid;
        int found = 0;
        int eperm = 0;
        for (int i = 0; i < NPROC; i++) {
            if (proc[i].state != UNUSED && proc[i].state != ZOMBIE &&
                proc[i].pgid == pgid) {
                if (!signal_send_allowed(&proc[i])) {
                    eperm = 1;
                    continue;
                }
                found = 1;
                /* BF: sig=0 is existence/permission check only */
                if (sig != 0)
                    kill_deliver(&proc[i], sig);
            }
        }
        if (found)
            return 0;
        return eperm ? (long)-1 : (long)-3; /* EPERM or ESRCH */
    }

    if (target_pid == -1) {
        /* Send to all processes except init (pid 1) and self */
        /* BF: Track found/eperm so we return correct POSIX error when
         * all eligible processes were skipped due to UID mismatch. */
        int found = 0;
        int eperm = 0;
        for (int i = 0; i < NPROC; i++) {
            if (proc[i].state != UNUSED && proc[i].state != ZOMBIE &&
                proc[i].pid != 1 && &proc[i] != current_proc) {
                if (!signal_send_allowed(&proc[i])) {
                    eperm = 1;
                    continue;
                }
                /* BF: sig=0 is existence/permission check only */
                if (sig != 0)
                    kill_deliver(&proc[i], sig);
                found = 1;
            }
        }
        if (found) return 0;
        return eperm ? (long)-1 : (long)-3; /* EPERM or ESRCH */
    }

    /* Positive pid: send to specific process */
    struct proc *target = find_signal_target(target_pid);
    if (!target) {
        return (long)-3; /* -ESRCH */
    }

    if (!signal_send_allowed(target))
        return (long)-1; /* -EPERM */

    /* BF: sig=0 is existence/permission check only -- no signal sent.
     * Prevents undefined behavior from signal_bit(0) = 1 << -1. */
    if (sig == 0)
        return 0;

    kill_deliver(target, sig);
    return 0;
}

/* ENH: rt_sigqueueinfo handler -- delivers a signal with siginfo_t data.
 * POSIX sigqueue() requires this syscall to send queued signals with
 * user-provided siginfo_t (including si_value). */
long sys_rt_sigqueueinfo_handler(struct trapframe *tf) {
    pid_t target_pid = (pid_t)(int)tf->a0;
    int sig = (int)tf->a1;
    uint64_t info_uaddr = tf->a2;

    if (sig < 0 || (!signal_is_valid(sig) && sig != 0)) {
        return (long)-22; /* -EINVAL */
    }

    struct proc *target = find_signal_target(target_pid);
    if (!target) {
        return (long)-3; /* -ESRCH */
    }

    if (!signal_send_allowed(target)) {
        return (long)-EPERM;
    }

    if (sig == 0)
        return 0; /* permission check only */

    /* BF: Store the user-provided siginfo_t in the shared sequence queue so
     * sigtimedwait and SA_SIGINFO delivery consume queued instances FIFO. */
    if (info_uaddr) {
        uint8_t kinfo[128];
        /* BF: Zero kinfo before copyin so bytes past K_SIGINFO_SIZE
         * are zeros, not uninitialized kernel stack data. */
        memset(kinfo, 0, sizeof(kinfo));
        if (copyin(current_proc->pagetable, kinfo, info_uaddr, K_SIGINFO_SIZE) < 0) {
            return (long)-EFAULT;
        }
        /* BF: A sender-provided si_signo must not let one queued slot
         * masquerade as another signal. */
        memcpy(kinfo + 0, &sig, 4);
        int qret = signal_queue_enqueue_raw(target, sig, kinfo);
        if (qret < 0)
            return (long)qret;
    }

    if (signal_is_realtime(sig)) {
        if (!(target->sigpending & signal_bit(sig))) {
            target->sig_sender[sig].si_pid = current_proc ? (int32_t)current_proc->pid : 0;
            target->sig_sender[sig].si_uid = 0;
            target->sig_sender[sig].si_code = K_SI_QUEUE;
            target->sig_sender[sig].si_status = 0;
            target->sig_sender[sig].valid = 1;
        }
        target->sigpending |= signal_bit(sig);
        if (target->state == SLEEPING)
            target->state = RUNNABLE;
    } else {
        /* BF: Standard signals still need kill_deliver's stop/continue and
         * wakeup behavior; queued siginfo, if any, was already recorded above. */
        kill_deliver(target, sig);
    }
    return 0;
}

long sys_rt_sigaction_handler(struct trapframe *tf) {
    int signum = (int)tf->a0;
    uint64_t act_uaddr = tf->a1;
    uint64_t oldact_uaddr = tf->a2;
    uint64_t sigsetsize = tf->a3;

    if (!rt_sigsetsize_valid(sigsetsize))
        return (long)-22;

    if (!signal_is_valid(signum) || signum == 9 || signum == 19) {
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
                return (long)-EFAULT;
            }
        }
        if (act_uaddr) {
            struct {
                uint64_t handler; uint64_t mask;
                int flags; int pad; uint64_t restorer;
            } act;
            if (copyin(current_proc->pagetable, &act, act_uaddr, sizeof(act)) < 0) {
                return (long)-EFAULT;
            }
            sig_sa[signum].handler = act.handler;
            /* BF: POSIX forbids blocking SIGKILL or SIGSTOP via sa_mask.
             * Strip these before storing so deliver_signal never masks them
             * when applying the handler's sa_mask to the process sigmask. */
            act.mask &= ~(signal_bit(SIGKILL) | signal_bit(SIGSTOP));
            sig_sa[signum].mask = act.mask;
            sig_sa[signum].flags = act.flags;
            sig_sa[signum].restorer = act.restorer;

            /* BF: POSIX: setting SIG_IGN for a signal clears any pending
             * and queued instance of that signal. */
            if (act.handler == 1) { /* SIG_IGN */
                current_proc->sigpending &= ~signal_bit(signum);
                signal_queue_purge_all(current_proc, signum);
            }
            /* BF: POSIX requires that setting SIG_DFL for signals whose default
             * action is "ignore" (SIGCHLD, SIGURG, SIGWINCH, SIGCONT) also
             * discards queued metadata. */
            if (act.handler == 0 && (signum == SIGCHLD || signum == SIGURG ||
                                     signum == SIGWINCH || signum == SIGCONT)) {
                current_proc->sigpending &= ~signal_bit(signum);
                signal_queue_purge_all(current_proc, signum);
            }
        }
        return (long)0;
    }
}

long sys_rt_sigprocmask_handler(struct trapframe *tf) {
    int how = (int)tf->a0;
    uint64_t set_uaddr = tf->a1;
    uint64_t oldset_uaddr = tf->a2;
    uint64_t sigsetsize = tf->a3;

    if (!rt_sigsetsize_valid(sigsetsize))
        return (long)-22;

    if (oldset_uaddr) {
        uint64_t oldmask = current_proc->sigmask;
        if (copyout(current_proc->pagetable, oldset_uaddr, &oldmask, sizeof(oldmask)) < 0) {
            return (long)-EFAULT;
        }
    }
    if (set_uaddr) {
        uint64_t newset = 0;
        if (copyin(current_proc->pagetable, &newset, set_uaddr, sizeof(newset)) < 0) {
            return (long)-EFAULT;
        }
        /* BF: POSIX forbids blocking SIGKILL or SIGSTOP.  Strip these from
         * any mask modification.  Also return -EINVAL for invalid how. */
        uint64_t unblockable = signal_bit(SIGKILL) | signal_bit(SIGSTOP);
        if (how == 0)       /* SIG_BLOCK */
            current_proc->sigmask |= (newset & ~unblockable);
        else if (how == 1)  /* SIG_UNBLOCK */
            current_proc->sigmask &= ~newset;
        else if (how == 2)  /* SIG_SETMASK */
            current_proc->sigmask = (newset & ~unblockable);
        else
            return (long)-22; /* EINVAL for invalid how */
    }
    return (long)0;
}

long sys_rt_sigreturn_handler(struct trapframe *tf) {
    uint64_t usp = tf->sp;

    struct signal_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (copyin(current_proc->pagetable, &frame, usp, sizeof(frame)) < 0) {
        return (long)-EFAULT;
    }

    current_proc->sigmask = frame.saved_sigmask;
    restore_user_signal_context(tf, &frame.saved_tf);

    /* BF: Clear SS_ONSTACK if the restored sp is outside the alternate
     * signal stack.  POSIX requires SS_ONSTACK to reflect whether the
     * thread is currently executing on the alt stack; without this,
     * a second signal after sigreturn would incorrectly think we're
     * still on the alt stack. */
    if (current_proc->ss_sp != 0 && current_proc->ss_size != 0) {
        uint64_t sp = tf->sp;
        if (sp < current_proc->ss_sp ||
            sp >= current_proc->ss_sp + current_proc->ss_size)
            current_proc->ss_flags &= ~1; /* clear SS_ONSTACK */
    }

    return SYSCALL_NORETURN;
}

long sys_rt_sigsuspend_handler(struct trapframe *tf) {
    if (!rt_sigsetsize_valid(tf->a1))
        return (long)-22;

    uint64_t mask;
    if (copyin(current_proc->pagetable, &mask, tf->a0, sizeof(uint64_t)) < 0)
        return -EFAULT; // EFAULT
    /* POSIX: SIGKILL and SIGSTOP are never blockable, even in temporary
     * sigsuspend masks. */
    mask &= ~(signal_bit(SIGKILL) | signal_bit(SIGSTOP));
    uint64_t old_mask = current_proc->sigmask;
    current_proc->sigmask = mask;
    /* BF: Suspend until a signal is deliverable under the temporary mask.
     * A signal still blocked by this mask may wake the task, but must not
     * end sigsuspend; the loop will simply go back to sleep. */
    while (1) {
        if (current_proc->sigpending & ~current_proc->sigmask)
            break;
        current_proc->state = SLEEPING;
        swtch(&current_proc->context, &scheduler_context);
    }

    /* BF: Defer mask restore via saved_sigmask_restore (same pattern
     * as ppoll/pselect6). deliver_signal will embed old_mask in the
     * signal frame, and sigreturn restores it after the handler. */
    current_proc->saved_sigmask_restore = old_mask;
    current_proc->has_saved_sigmask = 1;

    /* POSIX: sigsuspend must never be restarted by SA_RESTART.
     * Clear in_syscall to prevent deliver_signal from rewinding epc. */
    current_proc->in_syscall = 0;

    return -4; // EINTR
}

long sys_rt_sigpending_handler(struct trapframe *tf) {
    if (!rt_sigsetsize_valid(tf->a1))
        return (long)-22;

    uint64_t pending = current_proc->sigpending & current_proc->sigmask;
    if (copyout(current_proc->pagetable, tf->a0, &pending, sizeof(uint64_t)) < 0)
        return -EFAULT;
    return 0;
}

/* rt_sigtimedwait — synchronously wait for queued signals */
long sys_rt_sigtimedwait_handler(struct trapframe *tf) {
    /* a0 = sigset_t *set, a1 = siginfo_t *info, a2 = timespec *timeout, a3 = sigsetsize */
    uint64_t set_uaddr = tf->a0;
    uint64_t info_uaddr = tf->a1;
    uint64_t timeout_uaddr = tf->a2;
    uint64_t sigsetsize = tf->a3;

    if (!rt_sigsetsize_valid(sigsetsize))
        return (long)-22; /* EINVAL */

    if (!set_uaddr)
        return (long)-EFAULT; /* EFAULT */

    uint64_t waitset;
    if (copyin(current_proc->pagetable, &waitset, set_uaddr, sizeof(uint64_t)) < 0)
        return (long)-EFAULT; /* EFAULT */

    /* Parse timeout if provided */
    int64_t timeout_sec = 0, timeout_nsec = 0;
    int has_timeout = 0;
    if (timeout_uaddr) {
        struct { int64_t tv_sec; int64_t tv_nsec; } kts;
        if (copyin(current_proc->pagetable, &kts, timeout_uaddr, 16) < 0)
            return (long)-EFAULT; /* EFAULT */
        if (kts.tv_sec < 0 || kts.tv_nsec < 0 || kts.tv_nsec >= 1000000000L)
            return (long)-22; /* EINVAL */
        timeout_sec = kts.tv_sec;
        timeout_nsec = kts.tv_nsec;
        has_timeout = 1;
    }

    /* Calculate absolute deadline if timeout is provided */
    uint64_t deadline_ticks = 0;
    if (has_timeout) {
        uint64_t now = r_time();
        uint64_t timeout_ticks =
            timespec_to_ticks((uint64_t)timeout_sec, (uint64_t)timeout_nsec);
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
            if (current_proc->waiting_for_uart == 2)
                current_proc->waiting_for_uart = 0;

            uint8_t kinfo[128];
            int had_sender = current_proc->sig_sender[sig].valid;
            int found_queued = signal_queue_consume_one(current_proc, sig,
                                                        info_uaddr ? kinfo : NULL);
            if (found_queued) {
                current_proc->sig_sender[sig].valid = 0;
                if (signal_queue_has_more(current_proc, sig))
                    current_proc->sigpending |= signal_bit(sig);
            } else if (had_sender) {
                current_proc->sig_sender[sig].valid = 0;
            }

            /* Fill siginfo_t if requested */
            if (info_uaddr) {
                /* BF: Search sigqueue_buf for stored siginfo from sigqueue().
                 * This preserves si_value and si_code that the sender set.
                 * Without this, sigtimedwait always returned empty siginfo. */
                if (!found_queued) {
                    /* BF: Consume sig_sender metadata captured at generation
                     * time so sigtimedwait returns correct si_code/si_pid. */
                    memset(kinfo, 0, sizeof(kinfo));
                    *(int32_t *)(kinfo + 0) = (int32_t)sig; /* si_signo */
                    *(int32_t *)(kinfo + 4) = 0;            /* si_errno */
                    if (had_sender) {
                        *(int32_t *)(kinfo + 8) = current_proc->sig_sender[sig].si_code;
                        *(int32_t *)(kinfo + 12) = current_proc->sig_sender[sig].si_pid;
                        *(int32_t *)(kinfo + 16) = current_proc->sig_sender[sig].si_uid;
                        *(int32_t *)(kinfo + 20) = current_proc->sig_sender[sig].si_status;
                    }
                }
                /* BF: Only copy K_SIGINFO_SIZE (40) bytes to user — the user's
                 * siginfo_t is 40 bytes.  Writing 128 corrupts the caller's
                 * stack frame (ra, locals), causing epc=0x0 page faults. */
                if (copyout(current_proc->pagetable, info_uaddr, kinfo, K_SIGINFO_SIZE) < 0)
                    return (long)-EFAULT; /* EFAULT */
            }

            return (long)sig;
        }

        /* BF: POSIX specifies EAGAIN (not ETIMEDOUT) when sigtimedwait
         * times out without a signal becoming pending. */
        if (has_timeout && r_time() >= deadline_ticks) {
            if (current_proc->waiting_for_uart == 2)
                current_proc->waiting_for_uart = 0;
            return (long)-EAGAIN;
        }

        /* BF: Deadline-aware timer interrupts now handle near-term waits;
         * only spin inside one 100 Hz heartbeat to avoid sub-tick overshoot. */
        if (has_timeout) {
            uint64_t now = r_time();
            if (deadline_ticks > now && deadline_ticks - now <= TIMER_INTERVAL_TICKS)
                continue;
        }

        /* Sleep and retry.  Timed waits must join the timer wakeup path;
         * otherwise a pure timeout can sleep forever when no signal arrives. */
        if (has_timeout) {
            current_proc->sleep_until = deadline_ticks;
            current_proc->waiting_for_uart = 2;
            timer_reschedule();
        }
        current_proc->state = SLEEPING;
        swtch(&current_proc->context, &scheduler_context);
        if (has_timeout && current_proc->waiting_for_uart == 2)
            current_proc->waiting_for_uart = 0;

        /* Check for other signals that should interrupt the wait */
        uint64_t other_signals = current_proc->sigpending & ~waitset & ~current_proc->sigmask;
        if (other_signals)
            return (long)-EINTR;
    }
}

long sys_tgkill_handler(struct trapframe *tf) {
    pid_t tgid = (pid_t)tf->a0;
    pid_t tid = (pid_t)tf->a1;
    int sig = (int)tf->a2;

    if (tgid <= 0 || tid <= 0)
        return (long)-EINVAL;
    if (sig < 0 || (sig != 0 && !signal_is_valid(sig)))
        return (long)-EINVAL;

    struct proc *target = find_signal_target(tid);
    if (!target || target->pid != tgid)
        return (long)-ESRCH;

    if (!signal_send_allowed(target))
        return (long)-EPERM;

    if (sig == 0)
        return 0;

    kill_deliver(target, sig);
    return 0;
}

long sys_sigaltstack_handler(struct trapframe *tf) {
    uint64_t new_addr = tf->a0;
    uint64_t old_addr = tf->a1;

    /* stack_t layout: { uint64_t ss_sp; int32_t ss_flags; int32_t _pad; uint64_t ss_size; } = 24 bytes */
    if (old_addr != 0) {
        uint8_t kold[24];
        memset(kold, 0, sizeof(kold));
        *(uint64_t *)(kold + 0) = current_proc->ss_sp;
        int32_t old_flags = (int32_t)current_proc->ss_flags;
        /* BF: Dynamically compute SS_ONSTACK by checking if the current sp
         * is within the alt stack range.  The stored ss_flags may be stale
         * if deliver_signal set it but sigreturn didn't clear it properly. */
        if (current_proc->ss_sp != 0 && current_proc->ss_size != 0) {
            uint64_t sp = tf->sp;
            if (sp >= current_proc->ss_sp &&
                sp < current_proc->ss_sp + current_proc->ss_size)
                old_flags |= 1; /* SS_ONSTACK */
            else
                old_flags &= ~1;
        } else {
            /* BF: When no alt stack is configured (ss_sp==0 && ss_size==0),
             * POSIX requires SS_DISABLE (2) to be reported.  Without this,
             * tests checking the initial sigaltstack state would fail. */
            old_flags = 2; /* SS_DISABLE */
        }
        *(int32_t *)(kold + 8) = old_flags;
        *(uint64_t *)(kold + 16) = current_proc->ss_size;
        if (copyout(current_proc->pagetable, old_addr, kold, 24) < 0)
            return (long)-EFAULT;
    }

    if (new_addr != 0) {
        /* BF: Use dynamic SP check instead of stored ss_flags for the
         * on-stack test. A handler that longjmp'd out (skipping sigreturn)
         * would leave ss_flags with stale SS_ONSTACK, incorrectly blocking
         * future sigaltstack calls. */
        int on_stack = 0;
        if (current_proc->ss_sp != 0 && current_proc->ss_size != 0) {
            uint64_t sp = tf->sp;
            if (sp >= current_proc->ss_sp && sp < current_proc->ss_sp + current_proc->ss_size)
                on_stack = 1;
        }
        if (on_stack)
            return (long)-1; /* EPERM */

        uint8_t knew[24];
        if (copyin(current_proc->pagetable, knew, new_addr, 24) < 0)
            return (long)-EFAULT;

        int new_flags = (int)(*(int32_t *)(knew + 8));
        uint64_t new_size = *(uint64_t *)(knew + 16);

        /* BF: POSIX requires EINVAL if ss_flags contains values other
         * than SS_DISABLE (or zero).  SS_ONSTACK is not a valid input
         * flag — it is output-only.  Without this check, tests that
         * verify EINVAL for invalid flags would fail. */
        if (new_flags & ~2) /* any bit other than SS_DISABLE set */
            return (long)-22; /* EINVAL */

        /* BF: POSIX requires ENOMEM if enabling the alt stack with
         * ss_size < MINSIGSTKSZ.  Skip check when disabling. */
        if (!(new_flags & 2) && new_size < 2048) /* MINSIGSTKSZ = 2048 */
            return (long)-12; /* ENOMEM */

        if (new_flags & 2) {
            current_proc->ss_sp = 0;
            current_proc->ss_size = 0;
            current_proc->ss_flags = 2; /* SS_DISABLE */
        } else {
            current_proc->ss_sp = *(uint64_t *)(knew + 0);
            current_proc->ss_size = new_size;
            current_proc->ss_flags = 0;
        }
    }

    return 0;
}
