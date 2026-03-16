/*
 * time.c — measure command execution time.
 *
 * Usage: time COMMAND [ARGS...]
 * Prints real (wall-clock) elapsed time to stderr.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        dprintf(2, "usage: time command [args...]\n");
        return 1;
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: exec the command */
        execvp(argv[1], argv + 1);
        dprintf(2, "time: %s: command not found\n", argv[1]);
        _exit(127);
    }
    if (pid < 0) {
        dprintf(2, "time: fork failed\n");
        return 1;
    }

    int status;
    waitpid(pid, &status, 0);

    clock_gettime(CLOCK_MONOTONIC, &end);

    long sec = end.tv_sec - start.tv_sec;
    long nsec = end.tv_nsec - start.tv_nsec;
    if (nsec < 0) {
        sec--;
        nsec += 1000000000L;
    }
    long ms = nsec / 1000000;

    dprintf(2, "\nreal\t%ldm%ld.%03lds\n", sec / 60, sec % 60, ms);
    dprintf(2, "user\t0m0.000s\n");
    dprintf(2, "sys\t0m0.000s\n");

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 1;
}
