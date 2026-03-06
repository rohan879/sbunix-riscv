#include <stdio.h>
#include <syscall.h>

/*
 * ps — list processes via SYS_ps (custom syscall 500).
 *
 * Kernel fills an array of 24-byte records:
 *   offset 0:  int32_t  pid
 *   offset 4:  int32_t  ppid
 *   offset 8:  char     state  ('R', 'S', 'Z', '?')
 *   offset 9:  char     name[15]  (NUL-terminated, max 15 chars)
 */

#define MAX_PROCS 64

struct ps_entry {
    int   pid;
    int   ppid;
    char  state;
    char  name[15];
} __attribute__((packed));

/* pad to 24 bytes to match kernel layout */

int main(void) {
    /* use a 24-byte-stride buffer matching the kernel */
    static unsigned char buf[MAX_PROCS * 24];

    long n = syscall(SYS_ps, buf, MAX_PROCS);
    if (n < 0) {
        dprintf(2, "ps: syscall failed\n");
        return 1;
    }

    printf("  PID  PPID STAT COMMAND\n");
    for (long i = 0; i < n; i++) {
        unsigned char *e = buf + i * 24;
        int   pid   = (int)(e[0] | (e[1]<<8) | (e[2]<<16) | (e[3]<<24));
        int   ppid  = (int)(e[4] | (e[5]<<8) | (e[6]<<16) | (e[7]<<24));
        char  state = (char)e[8];
        char *name  = (char *)(e + 9);
        printf("%5d %5d %c    %s\n", pid, ppid, state, name);
    }
    return 0;
}
