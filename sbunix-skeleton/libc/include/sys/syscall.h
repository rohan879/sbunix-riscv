#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H

/* Re-export all syscall numbers from the main header */
#include <syscall.h>

/* POSIX aliases some tests may use */
#ifndef __NR_write
#define __NR_write       SYS_write
#define __NR_read        SYS_read
#define __NR_openat      SYS_openat
#define __NR_close       SYS_close
#define __NR_exit        SYS_exit
#define __NR_clone       SYS_clone
#define __NR_execve      SYS_execve
#define __NR_getpid      SYS_getpid
#define __NR_kill        SYS_kill
#define __NR_mmap        SYS_mmap
#define __NR_munmap      SYS_munmap
#define __NR_brk         SYS_brk
#endif

#endif /* _SYS_SYSCALL_H */
