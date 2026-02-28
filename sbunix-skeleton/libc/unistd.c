#include <unistd.h>
#include <syscall.h>
#include <stddef.h>

ssize_t read(int fd, void *buf, size_t count) {
    return syscall(SYS_read, fd, buf, count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    return syscall(SYS_write, fd, buf, count);
}

int open(const char *path, int flags) {
    return syscall(SYS_openat, AT_FDCWD, path, flags, 0);
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
