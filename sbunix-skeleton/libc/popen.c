/*
 * popen.c — popen/pclose for SBUnix libc.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#define MAX_POPEN 8

static struct {
    FILE *fp;
    pid_t pid;
} popen_table[MAX_POPEN];

FILE *popen(const char *command, const char *type) {
    int fds[2];
    int reading = (type[0] == 'r');

    if (pipe(fds) < 0)
        return NULL;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }

    if (pid == 0) {
        /* Child */
        if (reading) {
            close(fds[0]);
            dup2(fds[1], STDOUT_FILENO);
            close(fds[1]);
        } else {
            close(fds[1]);
            dup2(fds[0], STDIN_FILENO);
            close(fds[0]);
        }
        char *argv[4];
        argv[0] = "sh";
        argv[1] = "-c";
        argv[2] = (char *)command;
        argv[3] = NULL;
        execve("/bin/sh", argv, environ);
        _exit(127);
    }

    /* Parent */
    int parent_fd;
    if (reading) {
        close(fds[1]);
        parent_fd = fds[0];
    } else {
        close(fds[0]);
        parent_fd = fds[1];
    }

    FILE *fp = fdopen(parent_fd, type);
    if (!fp) {
        close(parent_fd);
        return NULL;
    }

    /* Record pid for pclose */
    for (int i = 0; i < MAX_POPEN; i++) {
        if (popen_table[i].fp == NULL) {
            popen_table[i].fp = fp;
            popen_table[i].pid = pid;
            break;
        }
    }

    return fp;
}

int pclose(FILE *stream) {
    pid_t pid = 0;

    for (int i = 0; i < MAX_POPEN; i++) {
        if (popen_table[i].fp == stream) {
            pid = popen_table[i].pid;
            popen_table[i].fp = NULL;
            popen_table[i].pid = 0;
            break;
        }
    }

    fclose(stream);

    if (pid == 0) {
        errno = ECHILD;
        return -1;
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }

    return status;
}
