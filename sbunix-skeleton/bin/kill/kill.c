#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <syscall.h>

int main(int argc, char *argv[]) {
    int sig = 15; /* SIGTERM default — kernel treats any signal as kill */
    int first_pid_arg = 1;

    if (argc < 2) {
        dprintf(2, "usage: kill [-<sig>] <pid> ...\n");
        return 1;
    }

    /* Optional signal number: -9, -15, etc. */
    if (argv[1][0] == '-' && argv[1][1] >= '0' && argv[1][1] <= '9') {
        sig = atoi(argv[1] + 1);
        first_pid_arg = 2;
    }

    if (first_pid_arg >= argc) {
        dprintf(2, "kill: no pid given\n");
        return 1;
    }

    int ret = 0;
    for (int i = first_pid_arg; i < argc; i++) {
        pid_t pid = (pid_t)atoi(argv[i]);
        if (pid <= 0) {
            dprintf(2, "kill: invalid pid: %s\n", argv[i]);
            ret = 1;
            continue;
        }
        if (syscall(SYS_kill, pid, sig) < 0) {
            dprintf(2, "kill: %d: no such process\n", (int)pid);
            ret = 1;
        }
    }
    return ret;
}
