#ifndef _ERRNO_H
#define _ERRNO_H

/*
 * errno — the thread-local error number set by syscall wrappers.
 * Since SBUnix is single-threaded per process this is a simple global.
 */
extern int errno;

/* Error numbers (Linux RISC-V ABI — matches <asm-generic/errno-base.h>) */
#define EPERM    1   /* operation not permitted */
#define ENOENT   2   /* no such file or directory */
#define ESRCH    3   /* no such process */
#define EINTR    4   /* interrupted system call */
#define EIO      5   /* input/output error */
#define ENXIO    6   /* no such device or address */
#define E2BIG    7   /* argument list too long */
#define ENOEXEC  8   /* exec format error */
#define EBADF    9   /* bad file descriptor */
#define ECHILD   10  /* no child processes */
#define EAGAIN   11  /* resource temporarily unavailable */
#define ENOMEM   12  /* out of memory */
#define EACCES   13  /* permission denied */
#define EFAULT   14  /* bad address */
#define ENOTBLK  15  /* block device required */
#define EBUSY    16  /* device or resource busy */
#define EEXIST   17  /* file exists */
#define EXDEV    18  /* invalid cross-device link */
#define ENODEV   19  /* no such device */
#define ENOTDIR  20  /* not a directory */
#define EISDIR   21  /* is a directory */
#define EINVAL   22  /* invalid argument */
#define ENFILE   23  /* too many open files in system */
#define EMFILE   24  /* too many open files */
#define ENOTTY   25  /* inappropriate ioctl for device */
#define ETXTBSY  26  /* text file busy */
#define EFBIG    27  /* file too large */
#define ENOSPC   28  /* no space left on device */
#define ESPIPE   29  /* illegal seek */
#define EROFS    30  /* read-only file system */
#define EMLINK   31  /* too many links */
#define EPIPE    32  /* broken pipe */
#define EDOM     33  /* numerical argument out of domain */
#define ERANGE   34  /* result too large */
#define EDEADLK  35  /* resource deadlock avoided */
#define ENAMETOOLONG 36  /* file name too long */
#define ENOLCK   37  /* no locks available */
#define ENOSYS   38  /* function not implemented */
#define ENOTEMPTY 39 /* directory not empty */
#define ELOOP    40  /* too many levels of symbolic links */
#define EWOULDBLOCK EAGAIN /* same as EAGAIN */
#define ENOMSG   42  /* no message of desired type */
#define EIDRM    43  /* identifier removed */
#define ENOSTR   60  /* device not a stream */
#define ENODATA  61  /* no data available */
#define ETIME    62  /* timer expired */
#define ENOSR    63  /* out of streams resources */
#define EREMOTE  66  /* object is remote */
#define ENOLINK  67  /* link has been severed */
#define EPROTO   71  /* protocol error */
#define EMULTIHOP 72 /* multihop attempted */
#define EBADMSG  74  /* bad message */
#define EOVERFLOW 75 /* value too large for defined data type */
#define EILSEQ   84  /* illegal byte sequence */
#define EUSERS   87  /* too many users */
#define ENOTSOCK 88  /* socket operation on non-socket */
#define EADDRINUSE 98   /* address already in use */
#define ECONNREFUSED 111 /* connection refused */
#define ETIMEDOUT  110  /* connection timed out */

/* Additional POSIX/Linux errno constants (asm-generic/errno.h) */
#define EDESTADDRREQ  89  /* destination address required */
#define EMSGSIZE      90  /* message too long */
#define EPROTOTYPE    91  /* protocol wrong type for socket */
#define ENOPROTOOPT   92  /* protocol not available */
#define EPROTONOSUPPORT 93 /* protocol not supported */
#define ESOCKTNOSUPPORT 94 /* socket type not supported */
#define EOPNOTSUPP    95  /* operation not supported on transport endpoint */
#define EPFNOSUPPORT  96  /* protocol family not supported */
#define EAFNOSUPPORT  97  /* address family not supported by protocol */
#define EADDRNOTAVAIL 99  /* cannot assign requested address */
#define ENETDOWN      100 /* network is down */
#define ENETUNREACH   101 /* network is unreachable */
#define ENETRESET     102 /* network dropped connection because of reset */
#define ECONNABORTED  103 /* software caused connection abort */
#define ECONNRESET    104 /* connection reset by peer */
#define ENOBUFS       105 /* no buffer space available */
#define EISCONN       106 /* transport endpoint is already connected */
#define ENOTCONN      107 /* transport endpoint is not connected */
#define EHOSTUNREACH  113 /* no route to host */
#define EALREADY      114 /* operation already in progress */
#define EINPROGRESS   115 /* operation now in progress */
#define ESTALE        116 /* stale file handle */
#define EDQUOT        122 /* quota exceeded */
#define ECANCELED     125 /* operation canceled */
#define ENOTSUP       126 /* operation not supported */
#define EOWNERDEAD    130 /* owner died */
#define ENOTRECOVERABLE 131 /* state not recoverable */

/* Convert raw negative kernel return to -1 + errno */
static inline long __syscall_ret(long r) {
    if (r < 0 && r >= -4095) {
        errno = (int)(-r);
        return -1;
    }
    return r;
}

#endif /* _ERRNO_H */
