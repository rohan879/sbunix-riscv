/*
 * stat.c — fstat/stat/lstat/mkdir/umask for SBUnix libc.
 */

#include <sys/stat.h>
#include <syscall.h>
#include <fcntl.h>
#include <errno.h>

int fstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    return (int)__syscall_ret(syscall(SYS_newfstatat, dirfd, path, buf, flags));
}

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

int mkdirat(int dirfd, const char *path, mode_t mode) {
    return (int)__syscall_ret(syscall(SYS_mkdirat, dirfd, path, mode));
}

int mkdir(const char *path, mode_t mode) {
    /* SYS_mkdirat = 34 */
    return (int)__syscall_ret(syscall(34, AT_FDCWD, path, mode));
}

int mknodat(int dirfd, const char *path, mode_t mode, dev_t dev) {
    return (int)__syscall_ret(syscall(SYS_mknodat, dirfd, path, mode, dev));
}

int mknod(const char *path, mode_t mode, dev_t dev) {
    /* SYS_mknodat = 33 */
    return (int)__syscall_ret(syscall(33, AT_FDCWD, path, mode, dev));
}

int mkfifo(const char *path, mode_t mode) {
    return mknodat(AT_FDCWD, path, (mode & 07777u) | S_IFIFO, 0);
}

int mkfifoat(int dirfd, const char *path, mode_t mode) {
    return mknodat(dirfd, path, (mode & 07777u) | S_IFIFO, 0);
}

mode_t umask(mode_t mask) {
    /* SYS_umask = 166 */
    return (mode_t)syscall(166, mask);
}

int chmod(const char *path, mode_t mode) {
    return (int)__syscall_ret(syscall(SYS_fchmodat, AT_FDCWD, path, mode, 0));
}

int fchmodat(int dirfd, const char *path, mode_t mode, int flags) {
    return (int)__syscall_ret(syscall(SYS_fchmodat, dirfd, path, mode, flags));
}

int fchmod(int fd, mode_t mode) {
    return (int)__syscall_ret(syscall(SYS_fchmod, fd, mode));
}

int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags) {
    return (int)__syscall_ret(syscall(SYS_utimensat, dirfd, path, times, flags));
}

int futimens(int fd, const struct timespec times[2]) {
    return utimensat(fd, NULL, times, 0);
}
