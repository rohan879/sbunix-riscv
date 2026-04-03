/*
 * resource.c — getrlimit, setrlimit, getrusage for SBUnix libc.
 */

#include <sys/resource.h>
#include <syscall.h>
#include <errno.h>
#include <string.h>

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

int getpriority(int which, id_t who) {
    (void)which;
    (void)who;
    errno = 0;
    return 0;
}

int setpriority(int which, id_t who, int prio) {
    (void)which;
    (void)who;
    (void)prio;
    return 0;
}
