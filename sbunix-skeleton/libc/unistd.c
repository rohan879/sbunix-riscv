#include <unistd.h>
#include <syscall.h>
#include <stddef.h>
#include <stdarg.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>

void __clock_after_fork_child(void);

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

int dup3(int oldfd, int newfd, int flags) {
    return (int)__syscall_ret(syscall(SYS_dup3, oldfd, newfd, flags));
}

int pipe(int fds[2]) {
    return (int)__syscall_ret(syscall(SYS_pipe2, fds, 0));
}

int pipe2(int fds[2], int flags) {
    return (int)__syscall_ret(syscall(SYS_pipe2, fds, flags));
}

pid_t fork(void) {
    long ret = __syscall_ret(syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0));
    /* BF: Kernel fork gives the child a fresh CPU-clock page; reset libc's
     * matching per-process clock() charge in the child copy as well. */
    if (ret == 0)
        __clock_after_fork_child();
    return (pid_t)ret;
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

int fexecve(int fd, char *const argv[], char *const envp[]) {
    return (int)__syscall_ret(syscall(SYS_fexecve, fd, argv, envp));
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

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    return (ssize_t)__syscall_ret(syscall(SYS_pread64, fd, buf, count, offset));
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    return (ssize_t)__syscall_ret(syscall(SYS_pwrite64, fd, buf, count, offset));
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

int sched_yield(void) {
    return (int)__syscall_ret(syscall(SYS_sched_yield));
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

int execl(const char *path, const char *arg, ...) {
    char *argv[64];
    int argc = 0;
    va_list ap;

    argv[argc++] = (char *)arg;

    va_start(ap, arg);
    while (argc < (int)(sizeof(argv) / sizeof(argv[0])) - 1) {
        char *next = va_arg(ap, char *);
        if (next == NULL)
            break;
        argv[argc++] = next;
    }
    va_end(ap);

    argv[argc] = NULL;
    if (argc == (int)(sizeof(argv) / sizeof(argv[0])) - 1)
        errno = E2BIG;

    return execve(path, argv, environ);
}

int execle(const char *path, const char *arg, ...) {
    char *argv[64];
    int argc = 0;
    va_list ap;
    char *const *envp;

    argv[argc++] = (char *)arg;

    va_start(ap, arg);
    while (argc < (int)(sizeof(argv) / sizeof(argv[0])) - 1) {
        char *next = va_arg(ap, char *);
        if (next == NULL)
            break;
        argv[argc++] = next;
    }

    envp = va_arg(ap, char *const *);
    va_end(ap);

    argv[argc] = NULL;
    if (argc == (int)(sizeof(argv) / sizeof(argv[0])) - 1)
        errno = E2BIG;
    if (envp == NULL)
        envp = environ;

    return execve(path, argv, (char *const *)envp);
}

int execlp(const char *file, const char *arg, ...) {
    char *argv[64];
    int argc = 0;
    va_list ap;

    argv[argc++] = (char *)arg;

    va_start(ap, arg);
    while (argc < (int)(sizeof(argv) / sizeof(argv[0])) - 1) {
        char *next = va_arg(ap, char *);
        if (next == NULL)
            break;
        argv[argc++] = next;
    }
    va_end(ap);

    argv[argc] = NULL;
    if (argc == (int)(sizeof(argv) / sizeof(argv[0])) - 1)
        errno = E2BIG;

    return execvp(file, argv);
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return (int)__syscall_ret(syscall(SYS_ioctl, fd, request, arg));
}

int isatty(int fd) {
    char buf[8];
    long ret = syscall(SYS_ioctl, fd, 0x5413, buf);
    if (ret == 0)
        return 1;
    /* BF: Do not assume stdio fds are terminals; fd validity and tty status
     * come from the kernel so EBADF and ENOTTY are distinguishable. */
    if (ret < 0)
        errno = (int)(-ret);
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

int setpgrp(void) {
    return setpgid(0, 0);
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

int fdatasync(int fd) {
    return fsync(fd);
}

int utimes(const char *path, const struct timeval times[2]) {
    struct timespec ts[2];
    if (times) {
        ts[0].tv_sec = times[0].tv_sec;
        ts[0].tv_nsec = times[0].tv_usec * 1000L;
        ts[1].tv_sec = times[1].tv_sec;
        ts[1].tv_nsec = times[1].tv_usec * 1000L;
        return utimensat(AT_FDCWD, path, ts, 0);
    }
    return utimensat(AT_FDCWD, path, NULL, 0);
}

void sync(void) {
    syscall(SYS_sync);
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

int waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options) {
    return (int)__syscall_ret(syscall(SYS_waitid, idtype, id, infop, options));
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

int fchownat(int dirfd, const char *path, uid_t owner, gid_t group, int flags) {
    (void)flags;
    (void)path;
    return (int)__syscall_ret(syscall(SYS_fchownat, dirfd, path, owner, group, flags));
}

int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags) {
    return (int)__syscall_ret(syscall(SYS_linkat, olddirfd, oldpath, newdirfd, newpath, flags));
}

int unlinkat(int dirfd, const char *path, int flags) {
    return (int)__syscall_ret(syscall(SYS_unlinkat, dirfd, path, flags));
}

int symlinkat(const char *target, int newdirfd, const char *linkpath) {
    return (int)__syscall_ret(syscall(SYS_symlinkat, target, newdirfd, linkpath));
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
    static char buf[256];
    int err = ttyname_r(fd, buf, sizeof(buf));
    if (err) {
        errno = err;
        return NULL;
    }
    return buf;
}

int ttyname_r(int fd, char *buf, size_t buflen) {
    char linkpath[64];
    char tmp[256];
    ssize_t len;
    int n;

    if (!buf) {
        errno = EINVAL;
        return EINVAL;
    }
    if (buflen == 0) {
        errno = ERANGE;
        return ERANGE;
    }
    if (!isatty(fd)) {
        int err = errno ? errno : ENOTTY;
        return err == EBADF ? EBADF : ENOTTY;
    }

    n = snprintf(linkpath, sizeof(linkpath), "/proc/self/fd/%d", fd);
    if (n < 0 || n >= (int)sizeof(linkpath)) {
        errno = ERANGE;
        return ERANGE;
    }

    len = readlink(linkpath, tmp, sizeof(tmp) - 1);
    if (len < 0) {
        int err = errno ? errno : ENOTTY;
        return err;
    }
    if (len >= (ssize_t)(sizeof(tmp) - 1)) {
        errno = ERANGE;
        return ERANGE;
    }
    if ((size_t)len >= buflen) {
        errno = ERANGE;
        return ERANGE;
    }
    memcpy(buf, tmp, (size_t)len);
    buf[len] = '\0';
    return 0;
}

long sysconf(int name) {
    switch (name) {
    case 30: return 4096;          /* _SC_PAGESIZE */
    case 4:  return 64;            /* _SC_OPEN_MAX */
    case 2:  return 100;           /* _SC_CLK_TCK */
    case 0:  return 131072;        /* _SC_ARG_MAX */
    case 3:  return 0;             /* _SC_NGROUPS_MAX */
    case 31: return 32;            /* _SC_TIMER_MAX */
    case 32: return 1;             /* _SC_REALTIME_SIGNALS */
    case 33: return 1;             /* _SC_MONOTONIC_CLOCK */
    case 34: return 1;             /* _SC_CPUTIME */
    case 35: return 1;             /* _SC_THREAD_CPUTIME */
    case 36: return 32;            /* _SC_SIGQUEUE_MAX */
    case 37: return 32;            /* _SC_DELAYTIMER_MAX */
    default: errno = EINVAL; return -1;
    }
}

long pathconf(const char *path, int name) {
    (void)path;
    if (name == _PC_FILESIZEBITS)
        return 64;
    if (name == _PC_NAME_MAX)
        return 255;
    if (name == _PC_PATH_MAX)
        return 4096;
    errno = EINVAL;
    return -1;
}

long fpathconf(int fd, int name) {
    (void)fd;
    if (name == _PC_FILESIZEBITS)
        return 64;
    if (name == _PC_NAME_MAX)
        return 255;
    if (name == _PC_PATH_MAX)
        return 4096;
    errno = EINVAL;
    return -1;
}

int chown(const char *path, uid_t owner, gid_t group) {
    (void)path; (void)owner; (void)group;
    return 0; /* single-user OS: always succeeds */
}

int lchown(const char *path, uid_t owner, gid_t group) {
    return fchownat(AT_FDCWD, path, owner, group, AT_SYMLINK_NOFOLLOW);
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

int setuid(uid_t uid) {
    return (int)__syscall_ret(syscall(SYS_setuid, uid));
}

int setgid(gid_t gid) {
    return (int)__syscall_ret(syscall(SYS_setgid, gid));
}

int seteuid(uid_t euid) {
    return setuid(euid);
}

int setegid(gid_t egid) {
    return setgid(egid);
}

int setresuid(uid_t ruid, uid_t euid, uid_t suid) {
    (void)suid;
    if (setreuid(ruid, euid) < 0)
        return -1;
    return 0;
}

int setresgid(gid_t rgid, gid_t egid, gid_t sgid) {
    (void)sgid;
    if (setregid(rgid, egid) < 0)
        return -1;
    return 0;
}

int getresuid(uid_t *ruid, uid_t *euid, uid_t *suid) {
    uid_t uid = getuid();
    uid_t e = geteuid();
    if (ruid) *ruid = uid;
    if (euid) *euid = e;
    if (suid) *suid = e;
    return 0;
}

int getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid) {
    gid_t gid = getgid();
    gid_t e = getegid();
    if (rgid) *rgid = gid;
    if (egid) *egid = e;
    if (sgid) *sgid = e;
    return 0;
}

int getgroups(int size, gid_t list[]) {
    if (size < 0) {
        errno = EINVAL;
        return -1;
    }
    if (size == 0)
        return 1;
    if (list == NULL) {
        errno = EFAULT;
        return -1;
    }
    list[0] = getgid();
    return 1;
}

long gethostid(void) {
    return 0;
}

int gethostname(char *name, size_t len) {
    const char *host = "sbunix";
    size_t need = strlen(host);

    if (!name) {
        errno = EFAULT;
        return -1;
    }
    if (len == 0) {
        errno = EINVAL;
        return -1;
    }
    if (need + 1 > len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(name, host);
    return 0;
}

int getlogin_r(char *buf, size_t buflen) {
    const char *name = "root";
    size_t len = strlen(name);

    if (!buf) {
        errno = EINVAL;
        return EINVAL;
    }
    if (len + 1 > buflen) {
        errno = ERANGE;
        return ERANGE;
    }
    strcpy(buf, name);
    return 0;
}

int getentropy(void *buffer, size_t length) {
    long ret = syscall(SYS_getrandom, buffer, length, 0);
    return (int)__syscall_ret(ret);
}

size_t confstr(int name, char *buf, size_t len) {
    const char *value;
    size_t need;

    if (name != _CS_PATH) {
        errno = EINVAL;
        return 0;
    }

    value = "/bin:/usr/bin";
    need = strlen(value) + 1;

    if (buf && len > 0) {
        size_t ncopy = need;
        if (ncopy > len)
            ncopy = len;
        memcpy(buf, value, ncopy - 1);
        buf[ncopy - 1] = '\0';
    }
    return need;
}

int lockf(int fd, int cmd, off_t len) {
    struct flock lk;

    lk.l_whence = SEEK_CUR;
    lk.l_start = 0;
    lk.l_len = len;
    lk.l_pid = 0;

    switch (cmd) {
    case F_ULOCK:
        lk.l_type = F_UNLCK;
        return fcntl(fd, F_SETLK, &lk);
    case F_LOCK:
        lk.l_type = F_WRLCK;
        return fcntl(fd, F_SETLKW, &lk);
    case F_TLOCK:
        lk.l_type = F_WRLCK;
        return fcntl(fd, F_SETLK, &lk);
    case F_TEST:
        lk.l_type = F_WRLCK;
        if (fcntl(fd, F_GETLK, &lk) < 0)
            return -1;
        if (lk.l_type == F_UNLCK || lk.l_type == F_RDLCK)
            return 0;
        errno = EACCES;
        return -1;
    default:
        errno = EINVAL;
        return -1;
    }
}

int posix_close(int fd, int flags) {
    (void)flags;
    return close(fd);
}

pid_t _Fork(void) {
    return fork();
}

char *crypt(const char *key, const char *salt) {
    static char out[32];
    size_t i;

    if (!key || !salt) {
        errno = EINVAL;
        return NULL;
    }

    out[0] = salt[0] ? salt[0] : 'x';
    out[1] = salt[1] ? salt[1] : 'x';
    for (i = 0; i < sizeof(out) - 3 && key[i]; i++)
        out[i + 2] = key[i];
    out[i + 2] = '\0';
    return out;
}

void setkey(const char *key) {
    (void)key;
}

void encrypt(char block[64], int edflag) {
    (void)block;
    (void)edflag;
}

void swab(const void *src, void *dest, ssize_t nbytes) {
    const unsigned char *s = (const unsigned char *)src;
    unsigned char *d = (unsigned char *)dest;
    ssize_t i;

    for (i = 0; i + 1 < nbytes; i += 2) {
        d[i] = s[i + 1];
        d[i + 1] = s[i];
    }
}
