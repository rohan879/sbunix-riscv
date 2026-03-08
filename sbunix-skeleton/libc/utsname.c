/*
 * utsname.c — uname() for SBUnix libc.
 */

#include <sys/utsname.h>
#include <string.h>

int uname(struct utsname *buf) {
    memset(buf, 0, sizeof(*buf));
    strcpy(buf->sysname,  "SBUnix");
    strcpy(buf->nodename, "sbunix");
    strcpy(buf->release,  "1.0.0");
    strcpy(buf->version,  "CSE506");
    strcpy(buf->machine,  "riscv64");
    return 0;
}
