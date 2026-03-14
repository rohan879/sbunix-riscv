/*
 * popen.c — pipe to/from a shell command for SBUnix libc.
 *
 * popen() forks, creates a pipe, and exec's "/bin/sh -c command".
 * pclose() closes the FILE and waits for the child to exit.
 */

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

/*
 * We need a small table to map FILE* back to the child PID for pclose().
 * Reuse the same pool size as fileio.c.
 */
#define POPEN_MAX 16

static struct {
    FILE *fp;
    pid_t pid;
} __popen_table[POPEN_MAX];

extern char **environ;

FILE *popen(const char *command, const char *type) {
    if (!command || !type)
        return (void *)0;

    /* Determine direction: "r" means we read from child's stdout,
     * "w" means we write to child's stdin */
    int reading;
    if (type[0] == 'r' && type[1] == '\0')
        reading = 1;
    else if (type[0] == 'w' && type[1] == '\0')
        reading = 0;
    else {
        errno = EINVAL;
        return (void *)0;
    }

    int fds[2];
    if (pipe(fds) < 0)
        return (void *)0;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return (void *)0;
    }

    if (pid == 0) {
        /* Child process */
        if (reading) {
            /* Parent reads from fds[0]; child writes to fds[1] */
            close(fds[0]);
            if (fds[1] != STDOUT_FILENO) {
                dup2(fds[1], STDOUT_FILENO);
                close(fds[1]);
            }
        } else {
            /* Parent writes to fds[1]; child reads from fds[0] */
            close(fds[1]);
            if (fds[0] != STDIN_FILENO) {
                dup2(fds[0], STDIN_FILENO);
                close(fds[0]);
            }
        }

        char *argv[4];
        argv[0] = "sh";
        argv[1] = "-c";
        argv[2] = (char *)command;
        argv[3] = (void *)0;
        execve("/bin/sh", argv, environ);
        _exit(127);
    }

    /* Parent process */
    FILE *fp;
    if (reading) {
        close(fds[1]);
        fp = fdopen(fds[0], "r");
    } else {
        close(fds[0]);
        fp = fdopen(fds[1], "w");
    }

    if (!fp) {
        /* fdopen failed; clean up */
        if (reading)
            close(fds[0]);
        else
            close(fds[1]);
        waitpid(pid, (void *)0, 0);
        return (void *)0;
    }

    /* Record the mapping */
    for (int i = 0; i < POPEN_MAX; i++) {
        if (__popen_table[i].fp == (void *)0) {
            __popen_table[i].fp = fp;
            __popen_table[i].pid = pid;
            return fp;
        }
    }

    /* Table full -- shouldn't happen normally */
    fclose(fp);
    waitpid(pid, (void *)0, 0);
    errno = EMFILE;
    return (void *)0;
}

int pclose(FILE *stream) {
    if (!stream) {
        errno = EINVAL;
        return -1;
    }

    /* Find the child PID */
    pid_t pid = -1;
    int slot = -1;
    for (int i = 0; i < POPEN_MAX; i++) {
        if (__popen_table[i].fp == stream) {
            pid = __popen_table[i].pid;
            slot = i;
            break;
        }
    }

    if (pid < 0) {
        errno = EINVAL;
        return -1;
    }

    /* Close the stream (this also closes the underlying fd) */
    fclose(stream);

    /* Clear the table entry */
    __popen_table[slot].fp = (void *)0;
    __popen_table[slot].pid = 0;

    /* Wait for the child */
    int status = 0;
    pid_t ret;
    do {
        ret = waitpid(pid, &status, 0);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0)
        return -1;

    return status;
}
