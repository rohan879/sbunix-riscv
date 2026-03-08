/*
 * resource.c — getrlimit, setrlimit, getrusage for SBUnix libc.
 */

#include <sys/resource.h>
#include <syscall.h>
#include <errno.h>
#include <string.h>

static inline long __syscall_ret(long r) {
    if (r < 0 && r >= -4095) {
        errno = (int)(-r);
        return -1;
    }
    return r;
}

int getrlimit(int resource, struct rlimit *rlim) {
    return (int)__syscall_ret(syscall(SYS_prlimit64, 0, resource, 0, rlim));
}

int setrlimit(int resource, const struct rlimit *rlim) {
    return (int)__syscall_ret(syscall(SYS_prlimit64, 0, resource, rlim, 0));
}

int getrusage(int who, struct rusage *usage) {
    (void)who;
    memset(usage, 0, sizeof(*usage));
    return 0;
}
