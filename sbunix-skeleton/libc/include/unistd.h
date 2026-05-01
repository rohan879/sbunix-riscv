#ifndef _UNISTD_H
#define _UNISTD_H

/* ENH: Advertise POSIX memory protection support so that Open POSIX
 * mmap tests 6-1, 6-2, 6-3 actually test PROT_NONE enforcement
 * instead of returning PTS_UNRESOLVED. */
#define _POSIX_MEMORY_PROTECTION 200112L

/* BF: Advertise POSIX CPU-time clock support.  The kernel tracks
 * per-process CPU time via cputime_ticks and services
 * CLOCK_PROCESS_CPUTIME_ID (clk_id 2) in clock_gettime. */
#define _POSIX_CPUTIME 200112L

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
int          usleep(unsigned long usecs);
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
int          getpagesize(void);
char   *getlogin(void);
int     nice(int inc);

/* ENH: POSIX version and feature test macros required by os-test include tests.
 * Values indicate the POSIX revision supported (200809L = POSIX.1-2008).
 * Features not implemented are set to -1 or 0 per POSIX convention. */
#define _POSIX_VERSION          200809L
#define _POSIX2_VERSION         200809L
#define _XOPEN_VERSION          700

/* POSIX feature advertising macros */
#define _POSIX_ADVISORY_INFO            (-1)
#define _POSIX_ASYNCHRONOUS_IO          (-1)
#define _POSIX_ASYNC_IO                 (-1)
#define _POSIX_BARRIERS                 (-1)
#define _POSIX_CHOWN_RESTRICTED         1
#define _POSIX_CLOCK_SELECTION          200809L
#define _POSIX_DEVICE_CONTROL           (-1)
#define _POSIX_FALLOC                   (-1)
#define _POSIX_FSYNC                    200809L
#define _POSIX_IPV6                     (-1)
#define _POSIX_JOB_CONTROL              1
#define _POSIX_MAPPED_FILES             200809L
#define _POSIX_MEMLOCK                  (-1)
#define _POSIX_MEMLOCK_RANGE            (-1)
#define _POSIX_MESSAGE_PASSING          (-1)
#define _POSIX_MONOTONIC_CLOCK          200809L
#define _POSIX_NO_TRUNC                 1
#define _POSIX_PRIO_IO                  (-1)
#define _POSIX_PRIORITIZED_IO           (-1)
#define _POSIX_PRIORITY_SCHEDULING      (-1)
#define _POSIX_RAW_SOCKETS              200809L
#define _POSIX_READER_WRITER_LOCKS      (-1)
#define _POSIX_REALTIME_SIGNALS         200809L
#define _POSIX_REGEXP                   1
#define _POSIX_SAVED_IDS                1
#define _POSIX_SEMAPHORES               (-1)
#define _POSIX_SHARED_MEMORY_OBJECTS    (-1)
#define _POSIX_SHELL                    1
#define _POSIX_SPAWN                    200809L
#define _POSIX_SPIN_LOCKS               (-1)
/* ENH: defer to sched.h if it already defined _POSIX_SPORADIC_SERVER */
#ifndef _POSIX_SPORADIC_SERVER
#define _POSIX_SPORADIC_SERVER          (-1)
#endif
#define _POSIX_SYNCHRONIZED_IO          (-1)
#define _POSIX_SYNC_IO                  (-1)
#define _POSIX_THREAD_ATTR_STACKADDR    (-1)
#define _POSIX_THREAD_ATTR_STACKSIZE    (-1)
#define _POSIX_THREAD_CPUTIME           (-1)
#define _POSIX_THREAD_PRIO_INHERIT      (-1)
#define _POSIX_THREAD_PRIO_PROTECT      (-1)
#define _POSIX_THREAD_PRIORITY_SCHEDULING (-1)
#define _POSIX_THREAD_PROCESS_SHARED    (-1)
#define _POSIX_THREAD_ROBUST_PRIO_INHERIT (-1)
#define _POSIX_THREAD_ROBUST_PRIO_PROTECT (-1)
#define _POSIX_THREAD_SAFE_FUNCTIONS    (-1)
#define _POSIX_THREADS                  (-1)
#define _POSIX_THREAD_SPORADIC_SERVER   (-1)
#define _POSIX_TIMEOUTS                 200809L
#define _POSIX_TIMERS                   200809L
#define _POSIX_TIMESTAMP_RESOLUTION     1
#define _POSIX_TYPED_MEMORY_OBJECTS     (-1)
#define _POSIX_VDISABLE                 0

/* POSIX V7/V8 environment macros */
#define _POSIX_V7_ILP32_OFF32           (-1)
#define _POSIX_V7_ILP32_OFFBIG          (-1)
#define _POSIX_V7_LP64_OFF64            1
#define _POSIX_V7_LPBIG_OFFBIG          (-1)
#define _POSIX_V8_ILP32_OFF32           (-1)
#define _POSIX_V8_ILP32_OFFBIG          (-1)
#define _POSIX_V8_LP64_OFF64            1
#define _POSIX_V8_LPBIG_OFFBIG          (-1)

/* POSIX.2 feature macros */
#define _POSIX2_C_BIND                  200809L
#define _POSIX2_C_DEV                   (-1)
#define _POSIX2_CHAR_TERM               1
#define _POSIX2_FORT_RUN                (-1)
#define _POSIX2_LOCALEDEF               (-1)
#define _POSIX2_SW_DEV                  (-1)
#define _POSIX2_SYMLINKS                1
#define _POSIX2_UPE                     (-1)

/* X/Open feature macros */
#define _XOPEN_CRYPT                    (-1)
#define _XOPEN_ENH_I18N                 (-1)
#define _XOPEN_REALTIME                 (-1)
#define _XOPEN_REALTIME_THREADS         (-1)
#define _XOPEN_SHM                      (-1)
#define _XOPEN_UNIX                     1
#define _XOPEN_UUCP                     (-1)

/* sysconf names -- ENH: added all POSIX-required _SC_* constants for
 * os-test compliance.  Values are sequential unique identifiers used by
 * the sysconf() implementation. */
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
#define _SC_2_C_BIND                38
#define _SC_2_C_DEV                 39
#define _SC_2_CHAR_TERM             40
#define _SC_2_FORT_RUN              41
#define _SC_2_LOCALEDEF             42
#define _SC_2_SW_DEV                43
#define _SC_2_UPE                   44
#define _SC_2_VERSION               45
#define _SC_ADVISORY_INFO           46
#define _SC_AIO_LISTIO_MAX          47
#define _SC_AIO_MAX                 48
#define _SC_AIO_PRIO_DELTA_MAX      49
#define _SC_ASYNCHRONOUS_IO         50
#define _SC_ATEXIT_MAX              51
#define _SC_BARRIERS                52
#define _SC_BC_BASE_MAX             53
#define _SC_BC_DIM_MAX              54
#define _SC_BC_SCALE_MAX            55
#define _SC_BC_STRING_MAX           56
#define _SC_CHILD_MAX               57
#define _SC_CLOCK_SELECTION         58
#define _SC_COLL_WEIGHTS_MAX        59
#define _SC_DEVICE_CONTROL          60
#define _SC_EXPR_NEST_MAX           61
#define _SC_FSYNC                   62
#define _SC_GETGR_R_SIZE_MAX        63
#define _SC_GETPW_R_SIZE_MAX        64
#define _SC_HOST_NAME_MAX           65
#define _SC_IOV_MAX                 66
#define _SC_IPV6                    67
#define _SC_JOB_CONTROL             68
#define _SC_LINE_MAX                69
#define _SC_LOGIN_NAME_MAX          70
#define _SC_MAPPED_FILES            71
#define _SC_MEMLOCK                 72
#define _SC_MEMLOCK_RANGE           73
#define _SC_MEMORY_PROTECTION       74
#define _SC_MESSAGE_PASSING         75
#define _SC_MQ_OPEN_MAX             76
#define _SC_MQ_PRIO_MAX             77
#define _SC_NPROCESSORS_CONF        78
#define _SC_NPROCESSORS_ONLN        79
#define _SC_NSIG                    80
#define _SC_PRIORITIZED_IO          81
#define _SC_PRIORITY_SCHEDULING     82
#define _SC_RAW_SOCKETS             83
#define _SC_READER_WRITER_LOCKS     84
#define _SC_RE_DUP_MAX              85
#define _SC_REGEXP                  86
#define _SC_RTSIG_MAX               87
#define _SC_SAVED_IDS               88
#define _SC_SEMAPHORES              89
#define _SC_SEM_NSEMS_MAX           90
#define _SC_SEM_VALUE_MAX           91
#define _SC_SHARED_MEMORY_OBJECTS   92
#define _SC_SHELL                   93
#define _SC_SPAWN                   94
#define _SC_SPIN_LOCKS              95
#define _SC_SPORADIC_SERVER         96
#define _SC_SS_REPL_MAX             97
#define _SC_STREAM_MAX              98
#define _SC_SYMLOOP_MAX             99
#define _SC_SYNCHRONIZED_IO         100
#define _SC_THREAD_ATTR_STACKADDR   101
#define _SC_THREAD_ATTR_STACKSIZE   102
#define _SC_THREAD_DESTRUCTOR_ITERATIONS 103
#define _SC_THREAD_KEYS_MAX         104
#define _SC_THREAD_PRIO_INHERIT     105
#define _SC_THREAD_PRIO_PROTECT     106
#define _SC_THREAD_PRIORITY_SCHEDULING 107
#define _SC_THREAD_PROCESS_SHARED   108
#define _SC_THREAD_ROBUST_PRIO_INHERIT 109
#define _SC_THREAD_ROBUST_PRIO_PROTECT 110
#define _SC_THREAD_SAFE_FUNCTIONS   111
#define _SC_THREADS                 112
#define _SC_THREAD_SPORADIC_SERVER  113
#define _SC_THREAD_STACK_MIN        114
#define _SC_THREAD_THREADS_MAX      115
#define _SC_TIMEOUTS                116
#define _SC_TIMERS                  117
#define _SC_TTY_NAME_MAX            118
#define _SC_TYPED_MEMORY_OBJECTS    119
#define _SC_TZNAME_MAX              120
#define _SC_V7_ILP32_OFF32          121
#define _SC_V7_ILP32_OFFBIG         122
#define _SC_V7_LP64_OFF64           123
#define _SC_V7_LPBIG_OFFBIG         124
#define _SC_V8_ILP32_OFF32          125
#define _SC_V8_ILP32_OFFBIG         126
#define _SC_V8_LP64_OFF64           127
#define _SC_V8_LPBIG_OFFBIG         128
#define _SC_VERSION                 129
#define _SC_XOPEN_CRYPT             130
#define _SC_XOPEN_ENH_I18N          131
#define _SC_XOPEN_REALTIME          132
#define _SC_XOPEN_REALTIME_THREADS  133
#define _SC_XOPEN_SHM               134
#define _SC_XOPEN_UNIX              135
#define _SC_XOPEN_UUCP              136
#define _SC_XOPEN_VERSION           137

/* pathconf names -- ENH: added all POSIX-required _PC_* constants for
 * os-test compliance.  Values are sequential identifiers used by the
 * pathconf()/fpathconf() implementation. */
#define _PC_FILESIZEBITS           18
#define _PC_NAME_MAX                3
#define _PC_PATH_MAX                4
#define _PC_2_SYMLINKS              5
#define _PC_ALLOC_SIZE_MIN          6
#define _PC_ASYNC_IO                7
#define _PC_CHOWN_RESTRICTED        8
#define _PC_FALLOC                  9
#define _PC_LINK_MAX               10
#define _PC_MAX_CANON              11
#define _PC_MAX_INPUT              12
#define _PC_NO_TRUNC               13
#define _PC_PIPE_BUF               14
#define _PC_PRIO_IO                15
#define _PC_REC_INCR_XFER_SIZE     16
#define _PC_REC_MAX_XFER_SIZE      17
#define _PC_REC_MIN_XFER_SIZE      19
#define _PC_REC_XFER_ALIGN         20
#define _PC_SYMLINK_MAX            21
#define _PC_SYNC_IO                22
#define _PC_TEXTDOMAIN_MAX         23
#define _PC_TIMESTAMP_RESOLUTION   24
#define _PC_VDISABLE               25

/* confstr names -- ENH: added POSIX-required _CS_* constants for os-test
 * compliance.  confstr() returns configuration strings by name. */
#define _CS_PATH                    0
#define _CS_POSIX_V7_ILP32_OFF32_CFLAGS     1
#define _CS_POSIX_V7_ILP32_OFF32_LDFLAGS    2
#define _CS_POSIX_V7_ILP32_OFF32_LIBS       3
#define _CS_POSIX_V7_ILP32_OFFBIG_CFLAGS    4
#define _CS_POSIX_V7_ILP32_OFFBIG_LDFLAGS   5
#define _CS_POSIX_V7_ILP32_OFFBIG_LIBS      6
#define _CS_POSIX_V7_LP64_OFF64_CFLAGS      7
#define _CS_POSIX_V7_LP64_OFF64_LDFLAGS     8
#define _CS_POSIX_V7_LP64_OFF64_LIBS        9
#define _CS_POSIX_V7_LPBIG_OFFBIG_CFLAGS   10
#define _CS_POSIX_V7_LPBIG_OFFBIG_LDFLAGS  11
#define _CS_POSIX_V7_LPBIG_OFFBIG_LIBS     12
#define _CS_POSIX_V7_THREADS_CFLAGS        13
#define _CS_POSIX_V7_THREADS_LDFLAGS       14
#define _CS_POSIX_V7_WIDTH_RESTRICTED_ENVS 15
#define _CS_POSIX_V8_ILP32_OFF32_CFLAGS    16
#define _CS_POSIX_V8_ILP32_OFF32_LDFLAGS   17
#define _CS_POSIX_V8_ILP32_OFF32_LIBS      18
#define _CS_POSIX_V8_ILP32_OFFBIG_CFLAGS   19
#define _CS_POSIX_V8_ILP32_OFFBIG_LDFLAGS  20
#define _CS_POSIX_V8_ILP32_OFFBIG_LIBS     21
#define _CS_POSIX_V8_LP64_OFF64_CFLAGS     22
#define _CS_POSIX_V8_LP64_OFF64_LDFLAGS    23
#define _CS_POSIX_V8_LP64_OFF64_LIBS       24
#define _CS_POSIX_V8_LPBIG_OFFBIG_CFLAGS   25
#define _CS_POSIX_V8_LPBIG_OFFBIG_LDFLAGS  26
#define _CS_POSIX_V8_LPBIG_OFFBIG_LIBS     27
#define _CS_POSIX_V8_THREADS_CFLAGS        28
#define _CS_POSIX_V8_THREADS_LDFLAGS       29
#define _CS_POSIX_V8_WIDTH_RESTRICTED_ENVS 30
#define _CS_V7_ENV                         31
#define _CS_V8_ENV                         32

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
