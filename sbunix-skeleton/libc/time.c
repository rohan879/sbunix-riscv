/*
 * time.c — time-related syscall wrappers for SBUnix libc.
 */

#include <time.h>
#include <syscall.h>

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
    long ret = syscall(SYS_clock_gettime, clk_id, tp);
    return (ret < 0) ? -1 : 0;
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
