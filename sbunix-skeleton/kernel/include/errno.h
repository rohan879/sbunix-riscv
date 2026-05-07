#ifndef _KERNEL_ERRNO_H
#define _KERNEL_ERRNO_H

/*
 * Kernel errno constants (Linux RISC-V ABI — matches asm-generic/errno-base.h
 * and libc/include/errno.h values).
 *
 * Unlike the libc version, there is no per-thread errno variable here.
 * Kernel code returns negative errno values directly (e.g. return -ENOMEM).
 */
#define EPERM         1   /* operation not permitted */
#define ENOENT        2   /* no such file or directory */
#define ESRCH         3   /* no such process */
#define EINTR         4   /* interrupted system call */
#define EIO           5   /* input/output error */
#define ENXIO         6   /* no such device or address */
#define E2BIG         7   /* argument list too long */
#define ENOEXEC       8   /* exec format error */
#define EBADF         9   /* bad file descriptor */
#define ECHILD       10   /* no child processes */
#define EAGAIN       11   /* resource temporarily unavailable */
#define ENOMEM       12   /* out of memory */
#define EACCES       13   /* permission denied */
#define EFAULT       14   /* bad address */
#define EBUSY        16   /* device or resource busy */
#define EEXIST       17   /* file exists */
#define EXDEV        18   /* invalid cross-device link */
#define ENODEV       19   /* no such device */
#define ENOTDIR      20   /* not a directory */
#define EISDIR       21   /* is a directory */
#define EINVAL       22   /* invalid argument */
#define ENFILE       23   /* too many open files in system */
#define EMFILE       24   /* too many open files */
#define ENOTTY       25   /* inappropriate ioctl for device */
#define EFBIG        27   /* file too large */
#define ENOSPC       28   /* no space left on device */
#define ESPIPE       29   /* illegal seek */
#define EROFS        30   /* read-only file system */
#define EMLINK       31   /* too many links */
#define EPIPE        32   /* broken pipe */
#define ERANGE       34   /* result too large */
#define ENAMETOOLONG 36   /* file name too long */
#define ENOSYS       38   /* function not implemented */
#define ENOTEMPTY    39   /* directory not empty */
#define ELOOP        40   /* too many levels of symbolic links */
#define EOVERFLOW    75   /* value too large for defined data type */
#define EILSEQ       84   /* illegal byte sequence */
#define ETIMEDOUT   110   /* connection timed out */

#endif /* _KERNEL_ERRNO_H */
