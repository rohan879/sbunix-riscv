/*
 * uio.c — readv/writev wrappers for SBUnix libc.
 */

#include <sys/uio.h>
#include <syscall.h>
#include <errno.h>

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    return __syscall_ret(syscall(SYS_readv, fd, iov, iovcnt));
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    return __syscall_ret(syscall(SYS_writev, fd, iov, iovcnt));
}
