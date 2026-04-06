#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

/* ENH: POSIX <sys/socket.h> header for os-test compliance.
 * SBUnix does not implement networking, but these definitions allow
 * test binaries to compile.  Functions are declared as stubs. */

#include <sys/types.h>
#include <sys/uio.h>

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

/* Socket types */
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_SEQPACKET 5
#define SOCK_CLOEXEC   02000000
#define SOCK_CLOFORK   040000000
#define SOCK_NONBLOCK  04000

/* Socket option levels */
#define SOL_SOCKET     1

/* Socket options for getsockopt/setsockopt */
#define SO_ACCEPTCONN  30
#define SO_BROADCAST   6
#define SO_DEBUG       1
#define SO_DOMAIN      39
#define SO_DONTROUTE   5
#define SO_ERROR       4
#define SO_KEEPALIVE   9
#define SO_LINGER      13
#define SO_OOBINLINE   10
#define SO_PROTOCOL    38
#define SO_RCVBUF      8
#define SO_RCVLOWAT    18
#define SO_RCVTIMEO    20
#define SO_REUSEADDR   2
#define SO_SNDBUF      7
#define SO_SNDLOWAT    19
#define SO_SNDTIMEO    21
#define SO_TYPE        3

/* Maximum listen backlog */
#define SOMAXCONN 128

/* Message flags for send/recv */
#define MSG_CMSG_CLOEXEC  0x40000000
#define MSG_CMSG_CLOFORK  0x80000000
#define MSG_CTRUNC   8
#define MSG_DONTROUTE 4
#define MSG_EOR      128
#define MSG_NOSIGNAL 0x4000
#define MSG_OOB      1
#define MSG_PEEK     2
#define MSG_TRUNC    32
#define MSG_WAITALL  0x100

/* Address families */
#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_INET   2
#define AF_INET6  10

/* Shutdown how values */
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

/* SCM rights for control messages */
#define SCM_RIGHTS 1

/* Socket address structures */
struct sockaddr {
  sa_family_t sa_family;
  char sa_data[14];
};

struct sockaddr_storage {
  sa_family_t ss_family;
  char __ss_padding[126];
};

struct linger {
  int l_onoff;
  int l_linger;
};

/* Control message header */
struct cmsghdr {
  socklen_t cmsg_len;
  int cmsg_level;
  int cmsg_type;
};

/* Message header for sendmsg/recvmsg */
struct msghdr {
  void *msg_name;
  socklen_t msg_namelen;
  struct iovec *msg_iov;
  int msg_iovlen;
  void *msg_control;
  socklen_t msg_controllen;
  int msg_flags;
};

/* CMSG macros */
#define CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_DATA(cmsg) ((unsigned char *)((struct cmsghdr *)(cmsg) + 1))
#define CMSG_SPACE(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))
#define CMSG_LEN(len)   (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))

#define CMSG_FIRSTHDR(mhdr) \
  ((mhdr)->msg_controllen >= sizeof(struct cmsghdr) \
   ? (struct cmsghdr *)(mhdr)->msg_control \
   : (struct cmsghdr *)0)

#define CMSG_NXTHDR(mhdr, cmsg) \
  ((cmsg)->cmsg_len < sizeof(struct cmsghdr) ? (struct cmsghdr *)0 : \
   ((unsigned char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len) + \
    sizeof(struct cmsghdr) > \
    (unsigned char *)(mhdr)->msg_control + (mhdr)->msg_controllen) \
   ? (struct cmsghdr *)0 \
   : (struct cmsghdr *)((unsigned char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len)))

/* Socket function declarations (stubs -- networking not implemented) */
int     socket(int domain, int type, int protocol);
int     socketpair(int domain, int type, int protocol, int sv[2]);
int     bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int     listen(int sockfd, int backlog);
int     accept(int sockfd, struct sockaddr *restrict addr,
               socklen_t *restrict addrlen);
int     accept4(int sockfd, struct sockaddr *restrict addr,
                socklen_t *restrict addrlen, int flags);
int     connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int     shutdown(int sockfd, int how);
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
ssize_t recvfrom(int sockfd, void *restrict buf, size_t len, int flags,
                 struct sockaddr *restrict src_addr,
                 socklen_t *restrict addrlen);
ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags);
int     getsockopt(int sockfd, int level, int optname,
                   void *restrict optval, socklen_t *restrict optlen);
int     setsockopt(int sockfd, int level, int optname,
                   const void *optval, socklen_t optlen);
int     getsockname(int sockfd, struct sockaddr *restrict addr,
                    socklen_t *restrict addrlen);
int     getpeername(int sockfd, struct sockaddr *restrict addr,
                    socklen_t *restrict addrlen);
int     sockatmark(int sockfd);

#endif /* _SYS_SOCKET_H */
