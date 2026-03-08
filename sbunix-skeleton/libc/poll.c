/*
 * poll.c — poll() stub for SBUnix libc.
 *
 * Stub: assumes all requested events are ready immediately.
 */

#include <poll.h>

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    (void)timeout;
    int ready = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        fds[i].revents = fds[i].events & (POLLIN | POLLOUT);
        if (fds[i].revents)
            ready++;
    }
    return ready;
}
