#pragma once
/*
 * RISC-V Linux syscall numbers.
 *
 * Reference:
 *   https://gpages.juszkiewicz.com.pl/syscalls-table/syscalls.html
 */

/* File system */
#define SYS_getcwd      17
#define SYS_dup3        24
#define SYS_chdir       49
#define SYS_openat      56
#define SYS_close       57
#define SYS_pipe2       59
#define SYS_getdents64  61
#define SYS_read        63
#define SYS_write       64
#define SYS_fstat       80
#define SYS_newfstatat  79

/* Process */
#define SYS_exit        93
#define SYS_exit_group  94
#define SYS_getpid      172
#define SYS_getppid     173
#define SYS_clone       220
#define SYS_execve      221
#define SYS_wait4       260

/* Signals */
#define SYS_kill        129
#define SYS_rt_sigaction  134
#define SYS_rt_sigprocmask 135

/* Time */
#define SYS_nanosleep   101
#define SYS_clock_gettime 113
#define SYS_gettimeofday  169

/* Memory */
#define SYS_brk         214
#define SYS_mmap        222
#define SYS_munmap      215
#define SYS_mprotect    226

/* I/O control */
#define SYS_ioctl       29
#define SYS_fcntl       25

/* Misc */
#define SYS_getuid      174
#define SYS_getgid      176

/* Flags & constants (kept here for backward compatibility) */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define AT_FDCWD  (-100)
#define SIGCHLD   17
#define WNOHANG   1

/* Wait macros (kept here for backward compatibility; also in <sys/wait.h>) */
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WIFEXITED(s)    (WTERMSIG(s) == 0)

long syscall(long num, ...);
