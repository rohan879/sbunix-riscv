#ifndef _POLL_H
#define _POLL_H

typedef unsigned int nfds_t;

struct pollfd {
    int   fd;
    short events;
    short revents;
};

#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020
/* ENH: POSIX-required priority band poll events for os-test compliance */
#define POLLRDNORM 0x0040
#define POLLRDBAND 0x0080
#define POLLWRNORM 0x0100
#define POLLWRBAND 0x0200

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#include <signal.h>
#include <time.h>
int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout,
          const sigset_t *sigmask);

#endif /* _POLL_H */
