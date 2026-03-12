/*
 * time.c — time-related syscall wrappers for SBUnix libc.
 */

#include <time.h>
#include <syscall.h>
#include <errno.h>

/*
 * nanosleep() — sleep for at least the duration specified by *req.
 * If interrupted, the remaining time is written to *rem (if non-NULL).
 */
int nanosleep(const struct timespec *req, struct timespec *rem) {
    return (int)syscall(SYS_nanosleep, req, rem);
}

/*
 * clock_gettime() — read a POSIX clock.
 * On SBUnix the kernel only handles CLOCK_REALTIME / CLOCK_MONOTONIC
 * via SYS_clock_gettime (113).  Returns 0 on success, -1 on error.
 */
int clock_gettime(int clk_id, struct timespec *tp) {
    return (int)__syscall_ret(syscall(SYS_clock_gettime, clk_id, tp));
}

/*
 * time() — return seconds since the Unix epoch.
 * Uses clock_gettime(CLOCK_REALTIME) as the underlying mechanism.
 * If t is non-NULL, the result is also stored in *t.
 */
time_t time(time_t *t) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
        return (time_t)-1;
    if (t)
        *t = ts.tv_sec;
    return ts.tv_sec;
}

/*
 * gettimeofday() — get time of day with microsecond resolution.
 * Wraps clock_gettime(CLOCK_REALTIME) and converts to struct timeval.
 */
int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv)
        return -1;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
        return -1;
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

int clock_nanosleep(int clockid, int flags,
                    const struct timespec *req, struct timespec *rem) {
    long ret = syscall(SYS_clock_nanosleep, clockid, flags, req, rem);
    if (ret < 0) { errno = (int)(-ret); return (int)(-ret); }
    return 0;
}
