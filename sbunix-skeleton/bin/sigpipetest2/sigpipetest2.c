/*
 * sigpipetest2 — SIGPIPE behaviour tests
 *
 * Tests that writing to a pipe with no readers:
 * - Default: kills the process with SIGPIPE
 * - With SIG_IGN: write returns -1 with errno EPIPE
 * - Multiple writes after close: each returns EPIPE
 */

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <errno.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * 1. default_sigpipe — writing to broken pipe kills child with SIGPIPE
 * ---------------------------------------------------------------------- */

static void test_default_sigpipe(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: create pipe, close read end, write to trigger SIGPIPE */
        int pfd[2];
        pipe(pfd);
        close(pfd[0]); /* close read end */
        write(pfd[1], "x", 1);
        /* Should not reach here — SIGPIPE kills us */
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK("default_sigpipe: child killed by signal", WIFSIGNALED(status));
    CHECK("default_sigpipe: WTERMSIG == SIGPIPE(13)",
          WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE);
}

/* -----------------------------------------------------------------------
 * 2. sigign_epipe — with SIG_IGN, write returns -1 and errno == EPIPE
 * ---------------------------------------------------------------------- */

static void test_sigign_epipe(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: ignore SIGPIPE, then write to broken pipe */
        struct sigaction sa;
        sa.sa_handler = SIG_IGN;
        sa.sa_mask = 0;
        sa.sa_flags = 0;
        sa.sa_restorer = 0;
        sigaction(SIGPIPE, &sa, 0);

        int pfd[2];
        pipe(pfd);
        close(pfd[0]); /* close read end */

        errno = 0;
        ssize_t ret = write(pfd[1], "x", 1);
        if (ret == -1 && errno == EPIPE)
            _exit(0);
        else
            _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK("sigign_epipe: child exited normally", WIFEXITED(status));
    CHECK("sigign_epipe: write returned -1 with EPIPE",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* -----------------------------------------------------------------------
 * 3. multiple_epipe — multiple writes after close all return EPIPE
 * ---------------------------------------------------------------------- */

static void test_multiple_epipe(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: ignore SIGPIPE, write multiple times */
        struct sigaction sa;
        sa.sa_handler = SIG_IGN;
        sa.sa_mask = 0;
        sa.sa_flags = 0;
        sa.sa_restorer = 0;
        sigaction(SIGPIPE, &sa, 0);

        int pfd[2];
        pipe(pfd);
        close(pfd[0]); /* close read end */

        int ok = 1;
        int i;
        for (i = 0; i < 3; i++) {
            errno = 0;
            ssize_t ret = write(pfd[1], "y", 1);
            if (ret != -1 || errno != EPIPE) {
                ok = 0;
                break;
            }
        }
        close(pfd[1]);
        _exit(ok ? 0 : 1);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK("multiple_epipe: child exited normally", WIFEXITED(status));
    CHECK("multiple_epipe: all 3 writes returned EPIPE",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* --- Main --- */

int main(void)
{
    printf("=== sigpipetest2: SIGPIPE behaviour ===\n");

    test_default_sigpipe();
    test_sigign_epipe();
    test_multiple_epipe();

    printf("=== sigpipetest2: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
