/*
 * stat.c — fstat/stat/lstat/mkdir/umask for SBUnix libc.
 */

#include <sys/stat.h>
#include <syscall.h>
#include <fcntl.h>
#include <errno.h>

int fstat(int fd, struct stat *buf) {
    return (int)__syscall_ret(syscall(SYS_fstat, fd, buf));
}

int stat(const char *path, struct stat *buf) {
    /* newfstatat(AT_FDCWD, path, buf, 0) */
    return (int)__syscall_ret(syscall(SYS_newfstatat, AT_FDCWD, path, buf, 0));
}

int lstat(const char *path, struct stat *buf) {
    /* AT_SYMLINK_NOFOLLOW = 0x100 */
    return (int)__syscall_ret(syscall(SYS_newfstatat, AT_FDCWD, path, buf, 0x100));
}

int mkdir(const char *path, mode_t mode) {
    /* SYS_mkdirat = 34 */
    return (int)__syscall_ret(syscall(34, AT_FDCWD, path, mode));
}

int mknod(const char *path, mode_t mode, dev_t dev) {
    /* SYS_mknodat = 33 */
    return (int)__syscall_ret(syscall(33, AT_FDCWD, path, mode, dev));
}

int fstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    return (int)__syscall_ret(syscall(SYS_newfstatat, dirfd, path, buf, flags));
}

mode_t umask(mode_t mask) {
    /* SYS_umask = 166 */
    return (mode_t)syscall(166, mask);
}

/*
 * statvfs / fstatvfs — filesystem statistics.
 * Stub: returns reasonable defaults since kernel lacks a statfs syscall.
 */
#include <sys/statvfs.h>
#include <string.h>

int statvfs(const char *path, struct statvfs *buf) {
    if (!path || !buf) {
        errno = 22; /* EINVAL */
        return -1;
    }
    /* Verify path exists */
    struct stat st;
    if (stat(path, &st) < 0)
        return -1;

    memset(buf, 0, sizeof(*buf));
    buf->f_bsize   = 4096;
    buf->f_frsize  = 4096;
    buf->f_blocks  = 4096;
    buf->f_bfree   = 2048;
    buf->f_bavail  = 2048;
    buf->f_files   = 1024;
    buf->f_ffree   = 512;
    buf->f_favail  = 512;
    buf->f_namemax = 255;
    return 0;
}

int fstatvfs(int fd, struct statvfs *buf) {
    if (!buf) {
        errno = 22;
        return -1;
    }
    /* Verify fd is valid via fstat */
    struct stat st;
    if (fstat(fd, &st) < 0)
        return -1;

    memset(buf, 0, sizeof(*buf));
    buf->f_bsize   = 4096;
    buf->f_frsize  = 4096;
    buf->f_blocks  = 4096;
    buf->f_bfree   = 2048;
    buf->f_bavail  = 2048;
    buf->f_files   = 1024;
    buf->f_ffree   = 512;
    buf->f_favail  = 512;
    buf->f_namemax = 255;
    return 0;
}
