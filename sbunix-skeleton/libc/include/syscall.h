#pragma once
/*
    RISC-V Linux syscall numbers

    NOTE: Syscall numbers used correspond to this
    reference here:
    https://gpages.juszkiewicz.com.pl/syscalls-table/syscalls.html
*/
#define SYS_getcwd 17
#define SYS_dup3 24
#define SYS_openat 56
#define SYS_close 57
#define SYS_pipe2 59
#define SYS_getdents64 61
#define SYS_read 63
#define SYS_write 64
#define SYS_exit 93
#define SYS_exit_group 94
#define SYS_getpid 172
#define SYS_clone 220
#define SYS_execve 221
#define SYS_wait4 260
#define SYS_chdir 49

/* Flags & constants */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define AT_FDCWD (-100)
#define SIGCHLD 17
#define WNOHANG 1

/* Wait macros */
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WTERMSIG(s) ((s) & 0x7f)
#define WIFEXITED(s) (WTERMSIG(s) == 0)

long syscall(long num, ...);
