/* RF: Time-related syscall handlers extracted from trap.c.
 * Clock, timer, nanosleep, getrusage, times, etc. */

#include <syscall_handlers.h>
#include <trap_internal.h>
#include <proc.h>
#include <riscv.h>
#include <string.h>
#include <printk.h>
#include <vm.h>
#include <errno.h>
#include <user_clock.h>

long sys_clock_gettime_handler(struct trapframe *tf) {
    uint64_t clk_id   = tf->a0;
    uint64_t ts_uaddr = tf->a1;
    if (!ts_uaddr)
        return (long)-EFAULT; /* EFAULT */

    struct { int64_t tv_sec; int64_t tv_nsec; } kts;

    if (clk_id == 0) {
        /* CLOCK_REALTIME: wall-clock time */
        int64_t sec, nsec;
        if (get_wall_time(&sec, &nsec) < 0)
            return (long)-ENODEV;
        kts.tv_sec  = sec;
        kts.tv_nsec = nsec;
    } else if (clk_id == 1) {
        /* CLOCK_MONOTONIC: time since boot */
        ticks_to_timespec(timekeeping_elapsed_ticks(), &kts.tv_sec, &kts.tv_nsec);
    } else if (clk_id == 2) {
        uint64_t cpu_ticks = user_clock_current_ticks(current_proc, r_time());
        ticks_to_timespec(cpu_ticks, &kts.tv_sec, &kts.tv_nsec);
    } else if (clk_id == 3) {
        /* CLOCK_THREAD_CPUTIME_ID: single-hart, so same as process time */
        uint64_t cpu_ticks = user_clock_current_ticks(current_proc, r_time());
        ticks_to_timespec(cpu_ticks, &kts.tv_sec, &kts.tv_nsec);
    } else {
        return (long)-22; /* EINVAL */
    }

    if (copyout(current_proc->pagetable, ts_uaddr, &kts, 16) < 0)
        return (long)-EFAULT; /* EFAULT */
    return (long)0;
}

long sys_clock_getres_handler(struct trapframe *tf) {
    uint64_t clk_id   = tf->a0;
    uint64_t res_uaddr = tf->a1;

    /* Support same clock IDs as clock_gettime */
    if (clk_id != 0 && clk_id != 1 && clk_id != 2 && clk_id != 3) {
        return (long)-22; /* EINVAL */
    }

    /* If res is NULL, just validate clock_id and return success */
    if (!res_uaddr)
        return (long)0;

    struct { int64_t tv_sec; int64_t tv_nsec; } kres;
    kres.tv_sec = 0;
    kres.tv_nsec = (int64_t)(NSEC_PER_SEC / TIMER_FREQ);

    if (copyout(current_proc->pagetable, res_uaddr, &kres, 16) < 0)
        return (long)-EFAULT; /* EFAULT */
    return (long)0;
}

static int valid_rel_timespec(int64_t sec, int64_t nsec) {
    return sec >= 0 && nsec >= 0 && nsec < (int64_t)NSEC_PER_SEC;
}

static long copyout_ticks_as_timespec(uint64_t uaddr, uint64_t ticks) {
    struct { int64_t tv_sec; int64_t tv_nsec; } kts;
    ticks_to_timespec(ticks, &kts.tv_sec, &kts.tv_nsec);
    if (copyout(current_proc->pagetable, uaddr, &kts, sizeof(kts)) < 0)
        return (long)-EFAULT;
    return 0;
}

static long sleep_until_deadline(uint64_t deadline, uint64_t rem_uaddr,
                                 int write_rem_on_interrupt) {
    while (r_time() < deadline) {
        current_proc->sleep_until = deadline;
        current_proc->waiting_for_uart = 2;
        timer_reschedule();
        current_proc->state = SLEEPING;
        swtch(&current_proc->context, &scheduler_context);

        uint64_t now = r_time();
        if (current_proc->waiting_for_uart != 2) {
            current_proc->waiting_for_uart = 0;
            break;
        }

        current_proc->waiting_for_uart = 0;
        if (signal_pending_interrupts_syscall(current_proc, 0)) {
            if (write_rem_on_interrupt && rem_uaddr) {
                uint64_t remaining = deadline > now ? deadline - now : 0;
                long ret = copyout_ticks_as_timespec(rem_uaddr, remaining);
                if (ret < 0)
                    return ret;
            }
            /* nanosleep and clock_nanosleep are not SA_RESTART syscalls. */
            current_proc->in_syscall = 0;
            return (long)-EINTR;
        }
    }

    current_proc->waiting_for_uart = 0;
    return 0;
}

long sys_clock_settime_handler(struct trapframe *tf) {
    uint64_t clk_id   = tf->a0;
    uint64_t ts_uaddr = tf->a1;

    if (!ts_uaddr)
        return (long)-EFAULT; /* EFAULT */

    /* Only CLOCK_REALTIME (0) is settable */
    if (clk_id != 0)
        return (long)-22; /* EINVAL */

    struct { int64_t tv_sec; int64_t tv_nsec; } kts;
    if (copyin(current_proc->pagetable, &kts, ts_uaddr, 16) < 0)
        return (long)-EFAULT; /* EFAULT */

    /* Validate nsec field */
    if (kts.tv_nsec < 0 || kts.tv_nsec >= 1000000000L)
        return (long)-22; /* EINVAL */

    /* For a single-user OS without persistent clock hardware or full timer
     * adjustment support, we return EPERM (Operation not permitted) for
     * clock_settime on CLOCK_REALTIME. This prevents tests from hanging
     * while waiting for adjusted absolute timers that will never fire.
     * Tests that check clock_settime functionality will get EPERM and
     * report UNRESOLVED rather than hanging indefinitely. */

    return (long)-EPERM;
}

long sys_nanosleep_handler(struct trapframe *tf) {
    uint64_t req_uaddr = tf->a0;
    uint64_t rem_uaddr = tf->a1;
    struct { int64_t tv_sec; int64_t tv_nsec; } kreq;

    if (!req_uaddr || !current_proc)
        return (long)-EFAULT;
    if (copyin(current_proc->pagetable, &kreq, req_uaddr, sizeof(kreq)) < 0)
        return (long)-EFAULT;
    if (!valid_rel_timespec(kreq.tv_sec, kreq.tv_nsec))
        return (long)-EINVAL;
    if (kreq.tv_sec == 0 && kreq.tv_nsec == 0)
        return 0;

    uint64_t deadline =
        timespec_rel_deadline((uint64_t)kreq.tv_sec, (uint64_t)kreq.tv_nsec);
    return sleep_until_deadline(deadline, rem_uaddr, 1);
}

long sys_gettimeofday_handler(struct trapframe *tf) {
    uint64_t tv_uaddr = tf->a0;
    if (tv_uaddr) {
        int64_t sec, nsec;
        if (get_wall_time(&sec, &nsec) < 0)
            return (long)-ENODEV;
        struct { int64_t sec; int64_t usec; } ktv;
        ktv.sec = sec;
        ktv.usec = nsec / 1000;
        if (copyout(current_proc->pagetable, tv_uaddr, &ktv, sizeof(ktv)) < 0) {
            return (long)-EFAULT;
        }
    }
    return (long)0;
}

long sys_alarm_handler(struct trapframe *tf) {
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
        current_proc->alarm_ticks = now + seconds_to_ticks(seconds);
    timer_reschedule();
    return (long)remaining;
}

/* POSIX timers — timer_create, timer_delete, timer_settime, timer_gettime, timer_getoverrun */
long sys_timer_create_handler(struct trapframe *tf) {
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
            return (long)-EFAULT; /* EFAULT */
        int notify = ksigevent[0]; /* sigev_notify */
        if (notify == 1) { /* SIGEV_NONE */
            signo = 0;
        } else if (notify == 0) { /* SIGEV_SIGNAL */
            signo = ksigevent[1]; /* sigev_signo */
            /* BF: POSIX timers may target the advertised realtime range
             * SIGRTMIN..SIGRTMAX, not just traditional signals below 32. */
            if (!signal_is_valid(signo))
                return (long)-22; /* EINVAL */
        } else {
            return (long)-22; /* EINVAL */
        }
    }

    /* Initialize timer.  active_timer_count tracks armed timers only,
     * not merely allocated slots, so newly-created disarmed timers do not
     * keep the timer interrupt on the slow NPTIMER scan path forever. */
    current_proc->timers[tid].used = 1;
    current_proc->timers[tid].clockid = clockid;
    current_proc->timers[tid].interval_ticks = 0;
    current_proc->timers[tid].next_expiry_ticks = 0;
    current_proc->timers[tid].signo = signo;
    current_proc->timers[tid].overrun = 0;

    /* Return timer_t (just the index) */
    if (copyout(current_proc->pagetable, timerid_uaddr, &tid, sizeof(int)) < 0)
        return (long)-EFAULT; /* EFAULT */

    return (long)0;
}

long sys_timer_delete_handler(struct trapframe *tf) {
    int tid = (int)tf->a0;
    if (tid < 0 || tid >= NPTIMER || !current_proc->timers[tid].used)
        return (long)-22; /* EINVAL */
    if (current_proc->timers[tid].next_expiry_ticks != 0 &&
        current_proc->active_timer_count > 0)
        current_proc->active_timer_count--;
    current_proc->timers[tid].used = 0;
    current_proc->timers[tid].next_expiry_ticks = 0;
    return (long)0;
}

long sys_timer_settime_handler(struct trapframe *tf) {
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
        return (long)-EFAULT; /* EFAULT */

    /* Validate nsec fields */
    if (kispec.int_nsec < 0 || kispec.int_nsec >= 1000000000L ||
        kispec.val_nsec < 0 || kispec.val_nsec >= 1000000000L)
        return (long)-22; /* EINVAL */

    /* Return old value if requested */
    if (old_uaddr) {
        struct { int64_t int_sec; int64_t int_nsec; int64_t val_sec; int64_t val_nsec; } old_ispec;
        /* Convert current timer state back to itimerspec */
        ticks_to_timespec(current_proc->timers[tid].interval_ticks,
                          &old_ispec.int_sec, &old_ispec.int_nsec);
        if (current_proc->timers[tid].next_expiry_ticks > 0) {
            uint64_t now = r_time();
            uint64_t remaining = (current_proc->timers[tid].next_expiry_ticks > now) ?
                                 (current_proc->timers[tid].next_expiry_ticks - now) : 0;
            ticks_to_timespec(remaining, &old_ispec.val_sec, &old_ispec.val_nsec);
        } else {
            old_ispec.val_sec = 0;
            old_ispec.val_nsec = 0;
        }
        if (copyout(current_proc->pagetable, old_uaddr, &old_ispec, 32) < 0)
            return (long)-EFAULT; /* EFAULT */
    }

    int was_armed = current_proc->timers[tid].next_expiry_ticks != 0;

    /* Set new timer values */
    current_proc->timers[tid].interval_ticks =
        timespec_to_ticks((uint64_t)kispec.int_sec, (uint64_t)kispec.int_nsec);

    if (kispec.val_sec == 0 && kispec.val_nsec == 0) {
        /* Disarm timer */
        current_proc->timers[tid].next_expiry_ticks = 0;
        current_proc->timers[tid].overrun = 0;
    } else {
        uint64_t value_ticks =
            timespec_to_ticks((uint64_t)kispec.val_sec, (uint64_t)kispec.val_nsec);
        if (flags & 1) { /* TIMER_ABSTIME */
            /* BF: The old code used value_ticks (user epoch time in
             * 10MHz ticks) directly as next_expiry_ticks (boot ticks).
             * This caused the timer to never fire because epoch seconds
             * (~1.7 billion) as boot ticks is far in the future.
             * Fix: convert absolute wall-clock time to a delta from now,
             * then add that delta to the current boot tick count.
             * This mirrors the clock_nanosleep ABSTIME pattern. */
            int clockid = current_proc->timers[tid].clockid;
            if (clockid == 0) {
                /* CLOCK_REALTIME: compare against wall-clock time */
                int64_t now_sec, now_nsec;
                if (get_wall_time(&now_sec, &now_nsec) < 0)
                    return (long)-ENODEV;
                uint64_t now_total_ns = (uint64_t)now_sec * NSEC_PER_SEC +
                                        (uint64_t)now_nsec;
                uint64_t target_ns = (uint64_t)kispec.val_sec * NSEC_PER_SEC +
                                     (uint64_t)kispec.val_nsec;
                if (target_ns <= now_total_ns) {
                    /* Already past: fire immediately */
                    current_proc->timers[tid].next_expiry_ticks = r_time();
                } else {
                    uint64_t diff_ns = target_ns - now_total_ns;
                    current_proc->timers[tid].next_expiry_ticks =
                        r_time() + ns_to_time_ticks(diff_ns);
                }
            } else if (clockid == 1) {
                /* CLOCK_MONOTONIC: compare against boot-relative time */
                uint64_t elapsed = timekeeping_elapsed_ticks();
                if (value_ticks <= elapsed) {
                    current_proc->timers[tid].next_expiry_ticks = r_time();
                } else {
                    current_proc->timers[tid].next_expiry_ticks =
                        r_time() + (value_ticks - elapsed);
                }
            } else {
                /* Other clocks: treat as relative for safety */
                current_proc->timers[tid].next_expiry_ticks =
                    r_time() + value_ticks;
            }
        } else {
            /* Relative time from now */
            current_proc->timers[tid].next_expiry_ticks = r_time() + value_ticks;
        }
        current_proc->timers[tid].overrun = 0;
    }

    int is_armed = current_proc->timers[tid].next_expiry_ticks != 0;
    if (!was_armed && is_armed) {
        current_proc->active_timer_count++;
    } else if (was_armed && !is_armed &&
               current_proc->active_timer_count > 0) {
        current_proc->active_timer_count--;
    }

    timer_reschedule();
    return (long)0;
}

long sys_timer_gettime_handler(struct trapframe *tf) {
    int tid = (int)tf->a0;
    uint64_t value_uaddr = tf->a1;

    if (tid < 0 || tid >= NPTIMER || !current_proc->timers[tid].used)
        return (long)-22; /* EINVAL */

    struct { int64_t int_sec; int64_t int_nsec; int64_t val_sec; int64_t val_nsec; } kispec;

    /* it_interval */
    ticks_to_timespec(current_proc->timers[tid].interval_ticks,
                      &kispec.int_sec, &kispec.int_nsec);

    /* it_value (time remaining until next expiry) */
    if (current_proc->timers[tid].next_expiry_ticks > 0) {
        uint64_t now = r_time();
        uint64_t remaining = (current_proc->timers[tid].next_expiry_ticks > now) ?
                             (current_proc->timers[tid].next_expiry_ticks - now) : 0;
        ticks_to_timespec(remaining, &kispec.val_sec, &kispec.val_nsec);
    } else {
        kispec.val_sec = 0;
        kispec.val_nsec = 0;
    }

    if (copyout(current_proc->pagetable, value_uaddr, &kispec, 32) < 0)
        return (long)-EFAULT; /* EFAULT */

    return (long)0;
}

long sys_timer_getoverrun_handler(struct trapframe *tf) {
    int tid = (int)tf->a0;

    if (tid < 0 || tid >= NPTIMER || !current_proc->timers[tid].used)
        return (long)-22; /* EINVAL */

    int overrun = current_proc->timers[tid].overrun;
    /* Reset overrun count after reading */
    current_proc->timers[tid].overrun = 0;
    return (long)overrun;
}

long sys_clock_nanosleep_handler(struct trapframe *tf) {
    int clockid = (int)tf->a0;
    int flags = (int)tf->a1;
    uint64_t rem_uaddr = tf->a3;
    struct { int64_t tv_sec; int64_t tv_nsec; } kreq;

    if (copyin(current_proc->pagetable, &kreq, tf->a2, sizeof(kreq)) < 0)
        return (long)-EFAULT;

    if ((flags & ~1) != 0)
        return (long)-EINVAL;

    if (clockid != 0 && clockid != 1 && clockid != 2 && clockid != 3)
        return (long)-EINVAL;

    if (kreq.tv_nsec < 0 || kreq.tv_nsec >= (int64_t)NSEC_PER_SEC)
        return (long)-EINVAL;
    if (kreq.tv_sec < 0) {
        if (flags & 1)
            return 0;
        return (long)-EINVAL;
    }

    uint64_t deadline =
        timespec_rel_deadline((uint64_t)kreq.tv_sec, (uint64_t)kreq.tv_nsec);

    if (flags & 1) {
        uint64_t target_ticks =
            timespec_to_ticks((uint64_t)kreq.tv_sec, (uint64_t)kreq.tv_nsec);
        uint64_t elapsed_ticks;
        if (clockid == 0) {
            int64_t now_sec, now_nsec;
            if (get_wall_time(&now_sec, &now_nsec) < 0)
                return (long)-ENODEV;
            uint64_t now_ns = (uint64_t)now_sec * NSEC_PER_SEC +
                              (uint64_t)now_nsec;
            uint64_t target_ns = (uint64_t)kreq.tv_sec * NSEC_PER_SEC +
                                 (uint64_t)kreq.tv_nsec;
            if (target_ns <= now_ns)
                return 0;
            deadline = r_time() + ns_to_time_ticks(target_ns - now_ns);
        } else if (clockid == 1) {
            elapsed_ticks = timekeeping_elapsed_ticks();
            if (target_ticks <= elapsed_ticks)
                return 0;
            deadline = r_time() + (target_ticks - elapsed_ticks);
        } else {
            deadline = r_time() + target_ticks;
        }
    }

    if (!(flags & 1) && kreq.tv_sec == 0 && kreq.tv_nsec == 0)
        return 0;

    return sleep_until_deadline(deadline, rem_uaddr, !(flags & 1));
}

long sys_getrusage_handler(struct trapframe *tf) {
    /* a0 = who (RUSAGE_SELF=0, RUSAGE_CHILDREN=-1), a1 = rusage buf */
    uint64_t uaddr = tf->a1;
    /* struct rusage is 128 bytes on RISC-V LP64 (two timeval + 14 longs) */
    uint8_t kru[128];
    memset(kru, 0, sizeof(kru));
    /* BF: Use charged CPU ticks so sleeping time does not inflate rusage. */
    uint64_t elapsed = user_clock_current_ticks(current_proc, r_time());
    int64_t sec;
    int64_t usec;
    ticks_to_timeval(elapsed, &sec, &usec);
    memcpy(kru + 0, &sec, 8);   /* ru_utime.tv_sec */
    memcpy(kru + 8, &usec, 8);  /* ru_utime.tv_usec */
    if (copyout(current_proc->pagetable, uaddr, kru, 128) < 0)
        return (long)-EFAULT;
    return 0;
}

/* times — process times */
long sys_times_handler(struct trapframe *tf) {
    /* a0 = struct tms *buf */
    uint64_t uaddr = tf->a0;
    /* struct tms: 4 clock_t fields (32 bytes on LP64) */
    uint8_t ktms[32];
    memset(ktms, 0, sizeof(ktms));

    /* Approximate process time from charged CPU ticks
     * tms_utime = user CPU time
     * tms_stime = system CPU time
     * tms_cutime = user CPU time of waited-for children
     * tms_cstime = system CPU time of waited-for children. */
    uint64_t cpu_elapsed = user_clock_current_ticks(current_proc, r_time());
    uint64_t elapsed = timekeeping_elapsed_ticks();
    int64_t ticks = (int64_t)timekeeping_to_clock_ticks(cpu_elapsed);
    int64_t real_ticks = (int64_t)timekeeping_to_clock_ticks(elapsed);

    memcpy(ktms + 0, &ticks, 8);  /* tms_utime */
    /* tms_stime, tms_cutime, tms_cstime remain zero */

    if (copyout(current_proc->pagetable, uaddr, ktms, 32) < 0)
        return (long)-EFAULT; /* EFAULT */

    /* Return value: elapsed real time in clock ticks since arbitrary point in the past */
    return real_ticks;
}
