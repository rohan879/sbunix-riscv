/*
 * stat.c — fstat/stat/lstat/mkdir/umask for SBUnix libc.
 */

#include <sys/stat.h>
#include <syscall.h>
#include <fcntl.h>

int fstat(int fd, struct stat *buf) {
    return (int)syscall(SYS_fstat, fd, buf);
}

int stat(const char *path, struct stat *buf) {
    /* newfstatat(AT_FDCWD, path, buf, 0) */
    return (int)syscall(SYS_newfstatat, AT_FDCWD, path, buf, 0);
}

int lstat(const char *path, struct stat *buf) {
    /* AT_SYMLINK_NOFOLLOW = 0x100 */
    return (int)syscall(SYS_newfstatat, AT_FDCWD, path, buf, 0x100);
}

int mkdir(const char *path, mode_t mode) {
    /* SYS_mkdirat = 34 */
    return (int)syscall(34, AT_FDCWD, path, mode);
}

mode_t umask(mode_t mask) {
    /* SYS_umask = 166 */
    return (mode_t)syscall(166, mask);
}
