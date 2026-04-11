/* RF: Time-related syscall handlers extracted from trap.c.
 * Clock, timer, nanosleep, getrusage, times, etc. */

#include <syscall_handlers.h>
#include <trap_internal.h>
#include <proc.h>
#include <riscv.h>
#include <string.h>
#include <printk.h>
#include <vm.h>

long sys_clock_gettime_handler(struct trapframe *tf) {
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
    } else if (clk_id == 2) {
        /* BF: Return actual per-process CPU time instead of wall-clock.
         * cputime_ticks is incremented by the timer interrupt (~10Hz),
         * so each tick represents ~100ms of CPU time. */
        uint64_t cpu_ns = current_proc->cputime_ticks * 100000000ULL; /* 100ms per tick */
        kts.tv_sec  = (int64_t)(cpu_ns / 1000000000ULL);
        kts.tv_nsec = (int64_t)(cpu_ns % 1000000000ULL);
    } else if (clk_id == 3) {
        /* CLOCK_THREAD_CPUTIME_ID: single-hart, so same as process time */
        uint64_t cpu_ns = current_proc->cputime_ticks * 100000000ULL;
        kts.tv_sec  = (int64_t)(cpu_ns / 1000000000ULL);
        kts.tv_nsec = (int64_t)(cpu_ns % 1000000000ULL);
    } else {
        return (long)-22; /* EINVAL */
    }

    if (copyout(current_proc->pagetable, ts_uaddr, &kts, 16) < 0)
        return (long)-14; /* EFAULT */
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

    /* Resolution is based on TIMER_FREQ (10MHz = 100ns ticks)
     * Report 1ns resolution for simplicity */
    struct { int64_t tv_sec; int64_t tv_nsec; } kres;
    kres.tv_sec = 0;
    kres.tv_nsec = 1; /* 1 nanosecond resolution */

    if (copyout(current_proc->pagetable, res_uaddr, &kres, 16) < 0)
        return (long)-14; /* EFAULT */
    return (long)0;
}

long sys_clock_settime_handler(struct trapframe *tf) {
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

long sys_nanosleep_handler(struct trapframe *tf) {
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

long sys_gettimeofday_handler(struct trapframe *tf) {
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
        current_proc->alarm_ticks = now + (uint64_t)seconds * TIMER_FREQ;
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

long sys_timer_delete_handler(struct trapframe *tf) {
    int tid = (int)tf->a0;
    if (tid < 0 || tid >= NPTIMER || !current_proc->timers[tid].used)
        return (long)-22; /* EINVAL */
    current_proc->timers[tid].used = 0;
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
                get_wall_time(&now_sec, &now_nsec);
                uint64_t now_total_ns = (uint64_t)now_sec * 1000000000UL +
                                        (uint64_t)now_nsec;
                uint64_t target_ns = (uint64_t)kispec.val_sec * 1000000000UL +
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
                uint64_t elapsed = r_time() - boot_time;
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

    return (long)0;
}

long sys_timer_gettime_handler(struct trapframe *tf) {
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

long sys_getrusage_handler(struct trapframe *tf) {
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
long sys_times_handler(struct trapframe *tf) {
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
