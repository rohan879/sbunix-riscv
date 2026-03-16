#include <unistd.h>
#include <syscall.h>
#include <stddef.h>
#include <stdarg.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>

ssize_t read(int fd, void *buf, size_t count) {
    return __syscall_ret(syscall(SYS_read, fd, buf, count));
}

ssize_t write(int fd, const void *buf, size_t count) {
    return __syscall_ret(syscall(SYS_write, fd, buf, count));
}

int open(const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int mode = va_arg(ap, int);
    va_end(ap);
    long ret = syscall(SYS_openat, AT_FDCWD, path, flags, mode);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

int close(int fd) {
    return (int)__syscall_ret(syscall(SYS_close, fd));
}

int dup2(int oldfd, int newfd) {
    if (oldfd == newfd) {
        /* POSIX: no-op if oldfd is valid */
        long r = syscall(SYS_fcntl, oldfd, 1 /* F_GETFD */);
        if (r < 0)
            return (int)__syscall_ret(r);
        return newfd;
    }
    return (int)__syscall_ret(syscall(SYS_dup3, oldfd, newfd, 0));
}

int pipe(int fds[2]) {
    return (int)__syscall_ret(syscall(SYS_pipe2, fds, 0));
}

pid_t fork(void) {
    return (pid_t)__syscall_ret(syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0));
}

pid_t waitpid(pid_t pid, int *status, int options) {
    return (pid_t)__syscall_ret(syscall(SYS_wait4, pid, status, options, 0));
}

pid_t getpid(void) {
    return syscall(SYS_getpid);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return (int)__syscall_ret(syscall(SYS_execve, path, argv, envp));
}

int chdir(const char *path) {
    return (int)__syscall_ret(syscall(SYS_chdir, path));
}

char *getcwd(char *buf, size_t size) {
    long ret = syscall(SYS_getcwd, buf, size);
    if (ret < 0)
        return NULL;
    return buf;
}

void _exit(int status) {
    syscall(SYS_exit_group, status);
    __builtin_unreachable();
}

unsigned int sleep(unsigned int seconds) {
    /* struct timespec { uint64_t tv_sec; uint64_t tv_nsec; } */
    unsigned long req[2] = { seconds, 0 };
    syscall(SYS_nanosleep, req, 0);
    return 0;
}

int dup(int oldfd) {
    return (int)__syscall_ret(syscall(SYS_fcntl, oldfd, 0 /* F_DUPFD */, 0));
}

off_t lseek(int fd, off_t offset, int whence) {
    return (off_t)__syscall_ret(syscall(SYS_lseek, fd, offset, whence));
}

pid_t getppid(void) {
    return (pid_t)syscall(SYS_getppid);
}

uid_t getuid(void) {
    return (uid_t)syscall(SYS_getuid);
}

gid_t getgid(void) {
    return (gid_t)syscall(SYS_getgid);
}

int usleep(unsigned int usecs) {
    unsigned long sec = usecs / 1000000;
    unsigned long nsec = (usecs % 1000000) * 1000UL;
    unsigned long req[2] = { sec, nsec };
    syscall(SYS_nanosleep, req, 0);
    return 0;
}

int unlink(const char *path) {
    return (int)__syscall_ret(syscall(SYS_unlinkat, AT_FDCWD, path, 0));
}

int rmdir(const char *path) {
    /* AT_REMOVEDIR = 0x200 */
    return (int)__syscall_ret(syscall(SYS_unlinkat, AT_FDCWD, path, 0x200));
}

int link(const char *oldpath, const char *newpath) {
    return (int)__syscall_ret(syscall(SYS_linkat, AT_FDCWD, oldpath, AT_FDCWD, newpath, 0));
}

int rename(const char *oldpath, const char *newpath) {
    return (int)__syscall_ret(syscall(SYS_renameat2, AT_FDCWD, oldpath, AT_FDCWD, newpath, 0));
}

int access(const char *path, int mode) {
    return (int)__syscall_ret(syscall(SYS_faccessat, AT_FDCWD, path, mode, 0));
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[]) {
    /* If file contains '/', use it directly */
    if (strchr(file, '/') != NULL)
        return execve(file, argv, environ);

    /* Search PATH for the executable */
    const char *path = NULL;
    if (environ) {
        for (char **ep = environ; *ep; ep++) {
            if (strncmp(*ep, "PATH=", 5) == 0) {
                path = *ep + 5;
                break;
            }
        }
    }
    if (!path)
        path = "/bin:/usr/bin";

    size_t filelen = strlen(file);
    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t dirlen = colon ? (size_t)(colon - p) : strlen(p);

        /* Build "dir/file" in buf; skip if it would overflow */
        char buf[256];
        if (dirlen + 1 + filelen + 1 <= sizeof(buf)) {
            strncpy(buf, p, dirlen);
            buf[dirlen] = '/';
            strcpy(buf + dirlen + 1, file);

            execve(buf, argv, environ);

            /* ENOENT: file not in this dir, try next */
            if (errno != 2)
                return -1;
        }

        if (!colon)
            break;
        p = colon + 1;
    }

    errno = 2; /* ENOENT */
    return -1;
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return (int)__syscall_ret(syscall(SYS_ioctl, fd, request, arg));
}

int isatty(int fd) {
    /* Try TIOCGWINSZ ioctl; if kernel supports it, fd is a tty */
    char buf[8];
    long ret = syscall(SYS_ioctl, fd, 0x5413, buf);
    if (ret == 0)
        return 1;
    /* Fallback: assume stdin/stdout/stderr are ttys */
    if (fd >= 0 && fd <= 2)
        return 1;
    return 0;
}

pid_t setsid(void) {
    return (pid_t)__syscall_ret(syscall(SYS_setsid));
}

pid_t getpgrp(void) {
    return (pid_t)syscall(SYS_getpgid, 0);
}

int setpgid(pid_t pid, pid_t pgid) {
    return (int)__syscall_ret(syscall(SYS_setpgid, pid, pgid));
}

pid_t tcgetpgrp(int fd) {
    pid_t pgrp;
    int ret = ioctl(fd, 0x540F, &pgrp); /* TIOCGPGRP */
    if (ret < 0) return (pid_t)-1;
    return pgrp;
}

int tcsetpgrp(int fd, pid_t pgrp) {
    return ioctl(fd, 0x5410, &pgrp); /* TIOCSPGRP */
}

pid_t getpgid(pid_t pid) {
    return (pid_t)__syscall_ret(syscall(SYS_getpgid, pid));
}

pid_t getsid(pid_t pid) {
    return (pid_t)__syscall_ret(syscall(SYS_getsid, pid));
}

int ftruncate(int fd, off_t length) {
    return (int)__syscall_ret(syscall(SYS_ftruncate, fd, length));
}

int truncate(const char *path, off_t length) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    int r = ftruncate(fd, length);
    close(fd);
    return r;
}

int fsync(int fd) {
    return (int)__syscall_ret(syscall(SYS_fsync, fd));
}

uid_t geteuid(void) {
    return (uid_t)syscall(SYS_geteuid);
}

gid_t getegid(void) {
    return (gid_t)syscall(SYS_getegid);
}

pid_t wait(int *status) {
    return waitpid(-1, status, 0);
}

unsigned int alarm(unsigned int seconds) {
    return (unsigned int)syscall(SYS_alarm, seconds);
}

int symlink(const char *target, const char *linkpath) {
    return (int)__syscall_ret(syscall(SYS_symlinkat, target, AT_FDCWD, linkpath));
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    return (ssize_t)__syscall_ret(syscall(SYS_readlinkat, AT_FDCWD, path, buf, bufsiz));
}

int openat(int dirfd, const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int mode = va_arg(ap, int);
    va_end(ap);
    long ret = syscall(SYS_openat, dirfd, path, flags, mode);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

int creat(const char *path, mode_t mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int faccessat(int dirfd, const char *path, int mode, int flags) {
    return (int)__syscall_ret(syscall(SYS_faccessat, dirfd, path, mode, flags));
}

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz) {
    return (ssize_t)__syscall_ret(syscall(SYS_readlinkat, dirfd, path, buf, bufsiz));
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    return (int)__syscall_ret(syscall(SYS_fcntl, fd, cmd, arg));
}

char *ttyname(int fd) {
    static char buf[32];
    if (!isatty(fd)) return NULL;
    /* SBUnix has a single console tty */
    strcpy(buf, "/dev/console");
    return buf;
}

int ttyname_r(int fd, char *buf, size_t buflen) {
    if (!isatty(fd)) { errno = ENOTTY; return ENOTTY; }
    const char *name = "/dev/console";
    size_t len = strlen(name);
    if (len + 1 > buflen) { errno = ERANGE; return ERANGE; }
    strcpy(buf, name);
    return 0;
}

long sysconf(int name) {
    switch (name) {
    case 30: return 4096;          /* _SC_PAGESIZE */
    case 4:  return 64;            /* _SC_OPEN_MAX */
    case 2:  return 100;           /* _SC_CLK_TCK */
    case 0:  return 131072;        /* _SC_ARG_MAX */
    case 3:  return 0;             /* _SC_NGROUPS_MAX */
    default: errno = EINVAL; return -1;
    }
}

int chown(const char *path, uid_t owner, gid_t group) {
    (void)path; (void)owner; (void)group;
    return 0; /* single-user OS: always succeeds */
}

int fchown(int fd, uid_t owner, gid_t group) {
    (void)fd; (void)owner; (void)group;
    return 0;
}

int fchdir(int fd) {
    return (int)__syscall_ret(syscall(SYS_fchdir, fd));
}

unsigned int getpagesize(void) {
    return 4096;
}

char *getlogin(void) {
    return "root";
}

int nice(int inc) {
    (void)inc;
    return 0; /* single-process priority: no-op */
}

int setreuid(uid_t ruid, uid_t euid) {
    (void)ruid; (void)euid;
    return 0; /* single-user OS */
}

int setregid(gid_t rgid, gid_t egid) {
    (void)rgid; (void)egid;
    return 0;
}
