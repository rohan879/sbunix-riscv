#include <sys/select.h>
#include <syscall.h>
#include <errno.h>
#include <stddef.h>

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout) {
    struct timespec ts, *pts = NULL;
    if (timeout) {
        ts.tv_sec = timeout->tv_sec;
        ts.tv_nsec = timeout->tv_usec * 1000L;
        pts = &ts;
    }
    return (int)__syscall_ret(syscall(SYS_pselect6, nfds, readfds, writefds,
                                      exceptfds, pts, 0));
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, const struct timespec *timeout,
            const sigset_t *sigmask) {
    /* pselect6 expects a pointer to {sigmask, sigsetsize} struct in a5 */
    unsigned long data[2] = { (unsigned long)sigmask, sizeof(sigset_t) };
    return (int)__syscall_ret(syscall(SYS_pselect6, nfds, readfds, writefds,
                                      exceptfds, timeout, data));
}
