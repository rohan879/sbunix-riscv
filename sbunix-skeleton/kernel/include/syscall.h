#ifndef _SYSCALL_H
#define _SYSCALL_H

// BUG: These syscall numbers don't match the libc's syscall.h which uses
// standard Linux RISC-V numbers (e.g. SYS_write=64, SYS_read=63,
// SYS_exit_group=94). User processes compiled against libc invoke the Linux
// numbers, so the kernel's ecall handler never matches them and reports
// "Unknown syscall". These values need to be updated to match
// libc/include/syscall.h.
#define SYS_write 1
#define SYS_exit  2
#define SYS_read  3
#define SYS_fork  4

#endif
