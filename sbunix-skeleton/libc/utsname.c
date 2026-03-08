/*
 * utsname.c — uname() for SBUnix libc.
 */

#include <sys/utsname.h>
#include <syscall.h>
#include <errno.h>

int uname(struct utsname *buf) {
    return (int)__syscall_ret(syscall(SYS_uname, buf));
}
