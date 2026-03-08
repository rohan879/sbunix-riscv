#ifndef _SYSCALL_H
#define _SYSCALL_H

/* RISC-V Linux syscall numbers (matching libc ABI) */

/* File system */
#define SYS_getcwd      17
#define SYS_dup3        24
#define SYS_chdir       49
#define SYS_openat      56
#define SYS_close       57
#define SYS_pipe2       59
#define SYS_getdents64  61
#define SYS_lseek       62
#define SYS_read        63
#define SYS_write       64
#define SYS_fstat       80
#define SYS_newfstatat  79

/* Process */
#define SYS_exit        93
#define SYS_exit_group  94
#define SYS_kill        129
#define SYS_getpid      172
#define SYS_getppid     173
#define SYS_getuid      174
#define SYS_getgid      176
#define SYS_clone       220
#define SYS_execve      221
#define SYS_wait4       260

/* Time */
#define SYS_nanosleep   101
#define SYS_clock_gettime 113

/* File control / access */
#define SYS_fcntl       25
#define SYS_ioctl       29
#define SYS_mkdirat     34
#define SYS_unlinkat    35
#define SYS_linkat      37
#define SYS_faccessat   48
#define SYS_umask       166
#define SYS_renameat2   276

/* Signal */
#define SYS_rt_sigaction   134
#define SYS_rt_sigprocmask 135
#define SYS_rt_sigreturn   139

/* Time */
#define SYS_gettimeofday   169

/* Memory */
#define SYS_brk         214
#define SYS_munmap      215
#define SYS_mmap        222
#define SYS_mprotect    226

/* Custom / teaching-OS extensions */
#define SYS_alarm       200   /* alarm(seconds) — one-shot SIGALRM */
#define SYS_ps          500   /* snapshot process table */

/* mmap flags (must match libc/include/sys/mman.h) */
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* AT_FDCWD for openat */
#define AT_FDCWD (-100)

/* clone flags */
#define SIGCHLD         17
#define CLONE_VM        0x00000100
#define CLONE_FILES     0x00000400
#define CLONE_SIGHAND   0x00000800

#endif
