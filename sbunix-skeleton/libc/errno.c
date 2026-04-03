/*
 * errno.c — errno global variable for SBUnix libc.
 *
 * In a real multi-threaded libc, errno would be a thread-local variable.
 * SBUnix processes are single-threaded, so a plain global is sufficient.
 *
 * Syscall wrappers set errno by inspecting the return value: if it is in
 * the range [-4095, -1], the syscall failed and errno = -retval.
 */

#include <errno.h>

int __errno_value = 0;
