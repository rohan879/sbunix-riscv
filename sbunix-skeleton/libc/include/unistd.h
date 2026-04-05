#ifndef _UNISTD_H
#define _UNISTD_H

/* ENH: Advertise POSIX memory protection support so that Open POSIX
 * mmap tests 6-1, 6-2, 6-3 actually test PROT_NONE enforcement
 * instead of returning PTS_UNRESOLVED. */
#define _POSIX_MEMORY_PROTECTION 200112L

#include <sys/types.h>
#include <sys/time.h>
#include <stdint.h>

#ifndef SEEK_SET
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2
#endif

#ifndef SEEK_DATA
#define SEEK_DATA 3
#endif

#ifndef SEEK_HOLE
#define SEEK_HOLE 4
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif

#ifndef O_CLOFORK
#define O_CLOFORK 040000000
#endif

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* access() mode bits */
#define F_OK  0  /* test for file existence */
#define X_OK  1  /* test for execute permission */
#define W_OK  2  /* test for write permission */
#define R_OK  4  /* test for read permission */

/* lseek() whence values */
#define SEEK_SET  0  /* seek from beginning of file */
#define SEEK_CUR  1  /* seek from current position */
#define SEEK_END  2  /* seek from end of file */

extern char **environ;

/* Basic I/O */
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int     open(const char *path, int flags, ...);
int     close(int fd);
int     dup(int oldfd);
int     dup2(int oldfd, int newfd);
int     dup3(int oldfd, int newfd, int flags);
int     pipe(int fds[2]);
int     pipe2(int fds[2], int flags);
off_t   lseek(int fd, off_t offset, int whence);
ssize_t pread(int fd, void *buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);

/* Process management */
pid_t   fork(void);
pid_t   getpid(void);
pid_t   getppid(void);
int     execve(const char *path, char *const argv[], char *const envp[]);
int     fexecve(int fd, char *const argv[], char *const envp[]);
int     execv(const char *path, char *const argv[]);
int     execvp(const char *file, char *const argv[]);
int     execl(const char *path, const char *arg, ...);
int     execle(const char *path, const char *arg, ...);
int     execlp(const char *file, const char *arg, ...);
pid_t   waitpid(pid_t pid, int *status, int options);
void    _exit(int status) __attribute__((noreturn));

/* Filesystem */
int     chdir(const char *path);
char   *getcwd(char *buf, size_t size);
int     unlink(const char *path);
int     rmdir(const char *path);
int     link(const char *oldpath, const char *newpath);
int     rename(const char *oldpath, const char *newpath);
int     access(const char *path, int mode);
int     symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);

/* Time */
unsigned int sleep(unsigned int seconds);
int          usleep(unsigned int usecs);
unsigned int alarm(unsigned int seconds);
int          utimes(const char *path, const struct timeval times[2]);

/* Miscellaneous */
uid_t   getuid(void);
gid_t   getgid(void);
uid_t   geteuid(void);
gid_t   getegid(void);

/* Terminal / process group */
int     isatty(int fd);
pid_t   setsid(void);
pid_t   getpgrp(void);
int     setpgid(pid_t pid, pid_t pgid);
int     setpgrp(void);
pid_t   tcgetpgrp(int fd);
int     tcsetpgrp(int fd, pid_t pgrp);
pid_t   getpgid(pid_t pid);
pid_t   getsid(pid_t pid);

/* File operations */
int     ftruncate(int fd, off_t length);
int     truncate(const char *path, off_t length);
int     fsync(int fd);
int     fdatasync(int fd);
void    sync(void);

/* Wait (convenience wrapper) */
pid_t   wait(int *status);

/* *at-style filesystem calls */
int     faccessat(int dirfd, const char *path, int mode, int flags);
int     fchownat(int dirfd, const char *path, uid_t owner, gid_t group, int flags);
int     linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags);
int     symlinkat(const char *target, int newdirfd, const char *linkpath);
int     unlinkat(int dirfd, const char *path, int flags);
ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz);

/* ioctl */
int     ioctl(int fd, unsigned long request, ...);

/* getopt */
extern char *optarg;
extern int   optind;
extern int   opterr;
extern int   optopt;
int     getopt(int argc, char *const argv[], const char *optstring);

/* Terminal */
char   *ttyname(int fd);
int     ttyname_r(int fd, char *buf, size_t buflen);

/* Process group / session */
int     setreuid(uid_t ruid, uid_t euid);
int     setregid(gid_t rgid, gid_t egid);
int     setuid(uid_t uid);
int     setgid(gid_t gid);
int     seteuid(uid_t euid);
int     setegid(gid_t egid);
int     setresuid(uid_t ruid, uid_t euid, uid_t suid);
int     setresgid(gid_t rgid, gid_t egid, gid_t sgid);
int     getresuid(uid_t *ruid, uid_t *euid, uid_t *suid);
int     getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid);
int     getgroups(int size, gid_t list[]);
int     lchown(const char *path, uid_t owner, gid_t group);
long    gethostid(void);
int     gethostname(char *name, size_t len);
int     getlogin_r(char *buf, size_t buflen);
int     getentropy(void *buffer, size_t length);

/* Misc POSIX */
long    sysconf(int name);
size_t  confstr(int name, char *buf, size_t len);
long    pathconf(const char *path, int name);
long    fpathconf(int fd, int name);
int     chown(const char *path, uid_t owner, gid_t group);
int     fchown(int fd, uid_t owner, gid_t group);
int     fchdir(int fd);
unsigned int getpagesize(void);
char   *getlogin(void);
int     nice(int inc);

/* sysconf names */
#define _SC_PAGESIZE       30
#define _SC_PAGE_SIZE      30
#define _SC_OPEN_MAX       4
#define _SC_CLK_TCK        2
#define _SC_ARG_MAX        0
#define _SC_NGROUPS_MAX    3
#define _SC_TIMER_MAX      31
#define _SC_REALTIME_SIGNALS 32
#define _SC_MONOTONIC_CLOCK 33
#define _SC_CPUTIME 34
#define _SC_THREAD_CPUTIME 35
#define _SC_SIGQUEUE_MAX 36
#define _SC_DELAYTIMER_MAX 37

/* pathconf names */
#define _PC_FILESIZEBITS   18
#define _PC_NAME_MAX        3
#define _PC_PATH_MAX        4

/* confstr names */
#define _CS_PATH           0

/* lockf commands */
#define F_ULOCK            0
#define F_LOCK             1
#define F_TLOCK            2
#define F_TEST             3
int     lockf(int fd, int cmd, off_t len);

int     posix_close(int fd, int flags);
#define POSIX_CLOSE_RESTART 0x1

/* Misc */
void    swab(const void *src, void *dest, ssize_t nbytes);
pid_t   _Fork(void);
char   *crypt(const char *key, const char *salt);
void    setkey(const char *key);
void    encrypt(char block[64], int edflag);

#endif /* _UNISTD_H */
