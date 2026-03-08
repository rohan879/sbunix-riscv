#include <poll.h>
#include <syscall.h>
#include <errno.h>
#include <stddef.h>

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    if (timeout < 0) {
        return (int)__syscall_ret(syscall(SYS_ppoll, fds, nfds, 0, 0, 0));
    }
    unsigned long ts[2];
    ts[0] = (unsigned long)(timeout / 1000);
    ts[1] = (unsigned long)((timeout % 1000) * 1000000L);
    return (int)__syscall_ret(syscall(SYS_ppoll, fds, nfds, ts, 0, 0));
}

int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout,
          const sigset_t *sigmask) {
    return (int)__syscall_ret(syscall(SYS_ppoll, fds, nfds, timeout, sigmask,
                                      sizeof(sigset_t)));
}
