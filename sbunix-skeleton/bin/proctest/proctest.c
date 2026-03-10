/*
 * proctest — cooperative multi-tasking test
 *
 * Exercises fork, execve, waitpid, pipe, dup2, getpid/getppid,
 * and address-space isolation.
 *
 * Run from the shell:  proctest
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* Global variable for address-space isolation test */
static int g_isolation = 1;

/* -----------------------------------------------------------------------
 * 1. fork_returns — child gets 0, parent gets >0
 * ---------------------------------------------------------------------- */

static void test_fork_returns(void) {
    pid_t pid = fork();
    if (pid == 0) {
        /* child */
        _exit(0);
    } else {
        CHECK("fork_returns: parent gets pid > 0", pid > 0);
        int status;
        waitpid(pid, &status, 0);
        CHECK("fork_returns: child got 0 (exited ok)", WIFEXITED(status));
    }
}

/* -----------------------------------------------------------------------
 * 2. fork_exec_echo — fork, child execve("/bin/echo"), parent waits
 * ---------------------------------------------------------------------- */

static void test_fork_exec_echo(void) {
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {"echo", "hello", NULL};
        char *envp[] = {NULL};
        execve("/bin/echo", argv, envp);
        _exit(127); /* execve failed */
    } else {
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK("fork_exec_echo: WIFEXITED", WIFEXITED(status));
        CHECK("fork_exec_echo: WEXITSTATUS == 0", WEXITSTATUS(status) == 0);
    }
}

/* -----------------------------------------------------------------------
 * 3. waitpid_status — child exits 42, parent checks status
 * ---------------------------------------------------------------------- */

static void test_waitpid_status(void) {
    pid_t pid = fork();
    if (pid == 0) {
        exit(42);
    } else {
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK("waitpid_status: WEXITSTATUS == 42", WEXITSTATUS(status) == 42);
    }
}

/* -----------------------------------------------------------------------
 * 4. waitpid_reap_any — waitpid(-1) returns the child pid
 * ---------------------------------------------------------------------- */

static void test_waitpid_reap_any(void) {
    pid_t child = fork();
    if (child == 0) {
        _exit(0);
    } else {
        int status = 0;
        pid_t reaped = waitpid(-1, &status, 0);
        CHECK("waitpid_reap_any: returned pid matches child", reaped == child);
    }
}

/* -----------------------------------------------------------------------
 * 5. exec_enoent — execve of nonexistent file returns -1
 * ---------------------------------------------------------------------- */

static void test_exec_enoent(void) {
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {"nonexistent", NULL};
        char *envp[] = {NULL};
        int ret = execve("/nonexistent", argv, envp);
        /* If execve returned, it failed */
        if (ret == -1)
            _exit(1);
        _exit(2);
    } else {
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK("exec_enoent: execve returned -1 (child exit 1)",
              WIFEXITED(status) && WEXITSTATUS(status) == 1);
    }
}

/* -----------------------------------------------------------------------
 * 6. getpid_getppid — child verifies pid relationships
 * ---------------------------------------------------------------------- */

static void test_getpid_getppid(void) {
    pid_t parent_pid = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        pid_t my_pid = getpid();
        pid_t my_ppid = getppid();
        int ok = (my_pid != my_ppid) && (my_ppid == parent_pid);
        _exit(ok ? 0 : 1);
    } else {
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK("getpid_getppid: child pid != ppid, ppid == parent",
              WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}

/* -----------------------------------------------------------------------
 * 7. fork_addr_isolation — parent's global unchanged after child modifies
 * ---------------------------------------------------------------------- */

static void test_fork_addr_isolation(void) {
    g_isolation = 1;
    pid_t pid = fork();
    if (pid == 0) {
        g_isolation = 2;
        _exit(0);
    } else {
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK("fork_addr_isolation: parent var still 1", g_isolation == 1);
    }
}

/* -----------------------------------------------------------------------
 * 8. multi_child_wait — fork 3 children, each exits differently
 * ---------------------------------------------------------------------- */

static void test_multi_child_wait(void) {
    int codes[3] = {10, 20, 30};
    pid_t pids[3];
    int i;

    for (i = 0; i < 3; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            _exit(codes[i]);
        }
    }

    int reaped = 0;
    for (i = 0; i < 3; i++) {
        int status = 0;
        pid_t w = waitpid(pids[i], &status, 0);
        if (w == pids[i] && WIFEXITED(status) &&
            WEXITSTATUS(status) == codes[i]) {
            reaped++;
        }
    }
    CHECK("multi_child_wait: all 3 children reaped", reaped == 3);
}

/* -----------------------------------------------------------------------
 * 9. fork_exec_args — exec echo with args, read output via pipe
 * ---------------------------------------------------------------------- */

static void test_fork_exec_args(void) {
    int pfd[2];
    int ret = pipe(pfd);
    if (ret != 0) {
        CHECK("fork_exec_args: pipe() failed", 0);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: redirect stdout to pipe write end */
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        char *argv[] = {"echo", "hello", "world", NULL};
        char *envp[] = {NULL};
        execve("/bin/echo", argv, envp);
        _exit(127);
    } else {
        /* Parent: read child's output from pipe */
        close(pfd[1]);
        char buf[128];
        int total = 0;
        int n;
        while ((n = read(pfd[0], buf + total,
                         sizeof(buf) - 1 - total)) > 0) {
            total += n;
        }
        buf[total] = '\0';
        close(pfd[0]);

        int status = 0;
        waitpid(pid, &status, 0);
        CHECK("fork_exec_args: output is 'hello world\\n'",
              strcmp(buf, "hello world\n") == 0);
    }
}

/* -----------------------------------------------------------------------
 * 10. pipe_fork_comm — parent writes to pipe, child reads and verifies
 * ---------------------------------------------------------------------- */

static void test_pipe_fork_comm(void) {
    int pfd[2];
    int ret = pipe(pfd);
    if (ret != 0) {
        CHECK("pipe_fork_comm: pipe() failed", 0);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: read from pipe and verify content */
        close(pfd[1]);
        char buf[16];
        int n = read(pfd[0], buf, sizeof(buf) - 1);
        close(pfd[0]);
        if (n == 4 && memcmp(buf, "test", 4) == 0)
            _exit(0);
        else
            _exit(1);
    } else {
        /* Parent: write to pipe */
        close(pfd[0]);
        write(pfd[1], "test", 4);
        close(pfd[1]);

        int status = 0;
        waitpid(pid, &status, 0);
        CHECK("pipe_fork_comm: child read 'test' correctly",
              WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}

/* -----------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    printf("=== proctest: cooperative multi-tasking ===\n");

    test_fork_returns();
    test_fork_exec_echo();
    test_waitpid_status();
    test_waitpid_reap_any();
    test_exec_enoent();
    test_getpid_getppid();
    test_fork_addr_isolation();
    test_multi_child_wait();
    test_fork_exec_args();
    test_pipe_fork_comm();

    printf("=== proctest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
