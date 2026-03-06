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

#endif /* _ERRNO_H */
