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
#define SYS_mknodat     33
#define SYS_mkdirat     34
#define SYS_unlinkat    35
#define SYS_symlinkat   36
#define SYS_linkat      37
#define SYS_umount2     39
#define SYS_mount       40
#define SYS_faccessat   48
#define SYS_chdir       49
#define SYS_fchdir      50
#define SYS_openat      56
#define SYS_close       57
#define SYS_pipe2       59
#define SYS_getdents64  61
#define SYS_lseek       62
#define SYS_read        63
#define SYS_write       64
#define SYS_newfstatat  79
#define SYS_fstat       80
#define SYS_renameat2   276

/* Process */
#define SYS_exit        93
#define SYS_exit_group  94
#define SYS_waitid      95
#define SYS_getpid      172
#define SYS_getppid     173
#define SYS_clone       220
#define SYS_execve      221
#define SYS_fexecve     281
#define SYS_wait4       260

/* Signals */
#define SYS_kill          129
#define SYS_sigaltstack   132
#define SYS_rt_sigsuspend 133
#define SYS_rt_sigaction  134
#define SYS_rt_sigprocmask 135
#define SYS_rt_sigpending 136
#define SYS_rt_sigtimedwait 137
#define SYS_rt_sigqueueinfo 138

/* Priorities */
#define SYS_getpriority   140
#define SYS_setpriority   141

/* Time */
#define SYS_nanosleep   101
#define SYS_timer_create 107
#define SYS_timer_gettime 108
#define SYS_timer_getoverrun 109
#define SYS_timer_settime 110
#define SYS_timer_delete 111
#define SYS_clock_settime 112
#define SYS_clock_gettime 113
#define SYS_clock_getres 114
#define SYS_clock_nanosleep 115
#define SYS_gettimeofday  169

/* Process times */
#define SYS_times         153

/* Memory */
#define SYS_brk         214
#define SYS_mmap        222
#define SYS_munmap      215
#define SYS_mprotect    226

/* Custom / simple-OS extensions */
#define SYS_alarm       200   /* alarm(seconds) — one-shot SIGALRM */
#define SYS_ps          500   /* snapshot process table */
#define SYS_reboot      142   /* reboot/poweroff (Linux RISC-V ABI) */

/* I/O control */
#define SYS_ioctl       29
#define SYS_fcntl       25

/* I/O vectors */
#define SYS_readv       65
#define SYS_writev      66

/* I/O multiplexing */
#define SYS_pselect6        72
#define SYS_ppoll           73

/* Symlinks */
#define SYS_readlinkat  78

/* Process groups / session */
#define SYS_set_tid_address 96
#define SYS_setpgid     154
#define SYS_getpgid     155
#define SYS_getsid      156
#define SYS_setsid      157

/* Misc */
#define SYS_umask       166
#define SYS_getuid      174
#define SYS_geteuid     175
#define SYS_getgid      176
#define SYS_getegid     177

/* Resource limits */
#define SYS_prlimit64   261
/* Random */
#define SYS_getrandom   278

/* Additional syscalls */
#define SYS_fchmod      52
#define SYS_fchmodat    53
#define SYS_sync        81
#define SYS_fsync       82
#define SYS_ftruncate   46
#define SYS_tgkill      131
#define SYS_rt_sigreturn 139
#define SYS_madvise     233
#define SYS_msync       227
#define SYS_futex       98
#define SYS_sched_yield 124
#define SYS_gettid      178
#define SYS_pread64     67
#define SYS_pwrite64    68
#define SYS_mremap      216
#define SYS_getrusage   165
#define SYS_epoll_create1 20
#define SYS_epoll_ctl     21
#define SYS_epoll_pwait   22
#define SYS_timerfd_create  85
#define SYS_timerfd_settime 86
#define SYS_timerfd_gettime 87
#define SYS_inotify_init1     26
#define SYS_inotify_add_watch 27
#define SYS_inotify_rm_watch  28
#define SYS_uname       160

/* Filesystem metadata */
#define SYS_statfs      43
#define SYS_fstatfs     44
#define SYS_utimensat   88

/* System V message queue */
#define SYS_msgget      186
#define SYS_msgctl      187
#define SYS_msgrcv      188
#define SYS_msgsnd      189

/* Ownership / credentials (stubs for single-user OS) */
#define SYS_fchownat    55
#define SYS_setgid      144
#define SYS_setuid      146

/* Flags & constants (kept here for backward compatibility) */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define AT_FDCWD  (-100)
#define SIGCHLD   17
#define WNOHANG   1

/* Wait macros live in <sys/wait.h> */

long syscall(long num, ...);
