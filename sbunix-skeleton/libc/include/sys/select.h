#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H

#include <signal.h>
#include <time.h>
#include <string.h>

typedef struct {
    unsigned long fds_bits[1024 / (8 * sizeof(unsigned long))];
} fd_set;

#define FD_SETSIZE 1024
#define FD_ZERO(s)     memset((s), 0, sizeof(fd_set))
#define FD_SET(fd, s)  ((s)->fds_bits[(fd)/(8*sizeof(unsigned long))] |= (1UL << ((fd)%(8*sizeof(unsigned long)))))
#define FD_CLR(fd, s)  ((s)->fds_bits[(fd)/(8*sizeof(unsigned long))] &= ~(1UL << ((fd)%(8*sizeof(unsigned long)))))
#define FD_ISSET(fd,s) (((s)->fds_bits[(fd)/(8*sizeof(unsigned long))] & (1UL << ((fd)%(8*sizeof(unsigned long))))) != 0)

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);
int pselect(int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, const struct timespec *timeout,
            const sigset_t *sigmask);

#endif /* _SYS_SELECT_H */
