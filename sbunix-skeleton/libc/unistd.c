#include <unistd.h>
#include <syscall.h>
#include <stddef.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/ioctl.h>

ssize_t read(int fd, void *buf, size_t count) {
    return syscall(SYS_read, fd, buf, count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    return syscall(SYS_write, fd, buf, count);
}

int open(const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int mode = va_arg(ap, int);
    va_end(ap);
    return syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

int close(int fd) {
    return syscall(SYS_close, fd);
}

int dup2(int oldfd, int newfd) {
    return syscall(SYS_dup3, oldfd, newfd, 0);
}

int pipe(int fds[2]) {
    return syscall(SYS_pipe2, fds, 0);
}

pid_t fork(void) {
    return syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0);
}

pid_t waitpid(pid_t pid, int *status, int options) {
    return syscall(SYS_wait4, pid, status, options, 0);
}

pid_t getpid(void) {
    return syscall(SYS_getpid);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return syscall(SYS_execve, path, argv, envp);
}

int chdir(const char *path) {
    return syscall(SYS_chdir, path);
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
    /* Find the lowest available fd >= 0 by trying dup3 into increasing slots.
     * We try slots 0..15; skip if they'd be the same as oldfd. */
    for (int newfd = 0; newfd < 16; newfd++) {
        if (newfd == oldfd) continue;
        /* Probe: try dup3(oldfd, newfd). If newfd is already open the kernel
         * closes it and reuses it — that's correct dup semantics. */
        long r = syscall(SYS_dup3, oldfd, newfd, 0);
        if (r == newfd) return newfd;
    }
    return -1;
}

off_t lseek(int fd, off_t offset, int whence) {
    return (off_t)syscall(SYS_lseek, fd, offset, whence);
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
    unsigned long req[2] = { 0, (unsigned long)usecs * 1000UL };
    syscall(SYS_nanosleep, req, 0);
    return 0;
}

int unlink(const char *path) {
    return (int)syscall(SYS_unlinkat, AT_FDCWD, path, 0);
}

int rmdir(const char *path) {
    /* AT_REMOVEDIR = 0x200 */
    return (int)syscall(SYS_unlinkat, AT_FDCWD, path, 0x200);
}

int link(const char *oldpath, const char *newpath) {
    return (int)syscall(SYS_linkat, AT_FDCWD, oldpath, AT_FDCWD, newpath, 0);
}

int rename(const char *oldpath, const char *newpath) {
    return (int)syscall(SYS_renameat2, AT_FDCWD, oldpath, AT_FDCWD, newpath, 0);
}

int access(const char *path, int mode) {
    return (int)syscall(SYS_faccessat, AT_FDCWD, path, mode, 0);
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[]) {
    /* Simple implementation: only searches absolute/relative paths */
    return execve(file, argv, environ);
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return (int)syscall(SYS_ioctl, fd, request, arg);
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
    /* No kernel support yet -- return current PID as stub */
    return getpid();
}

pid_t getpgrp(void) {
    return getpid();
}

int setpgid(pid_t pid, pid_t pgid) {
    (void)pid; (void)pgid;
    return 0;
}

pid_t tcgetpgrp(int fd) {
    (void)fd;
    return getpid();
}

int tcsetpgrp(int fd, pid_t pgrp) {
    (void)fd; (void)pgrp;
    return 0;
}

unsigned int alarm(unsigned int seconds) {
    return (unsigned int)syscall(SYS_alarm, seconds);
}
