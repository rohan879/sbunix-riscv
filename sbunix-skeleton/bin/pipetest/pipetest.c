/*
 * pipetest.c -- Pipe capacity and blocking tests for SBUnix.
 *
 * Exercises:
 *   1. Pipe basic read/write
 *   2. Pipe EOF (close write end, read returns 0)
 *   3. SIGPIPE on write to closed read end
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else      { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * 1. Pipe basic read/write
 * ---------------------------------------------------------------------- */

static void test_pipe_basic_rw(void) {
    int fds[2];
    int r = pipe(fds);
    CHECK("pipe: pipe() returns 0", r == 0);
    if (r != 0) return;

    const char *msg = "pipe_hello";
    ssize_t nw = write(fds[1], msg, 10);
    CHECK("pipe: write 10 bytes", nw == 10);

    char buf[32];
    memset(buf, 0, sizeof(buf));
    ssize_t nr = read(fds[0], buf, sizeof(buf));
    CHECK("pipe: read 10 bytes back", nr == 10);
    CHECK("pipe: data matches", memcmp(buf, "pipe_hello", 10) == 0);

    close(fds[0]);
    close(fds[1]);
}

/* -----------------------------------------------------------------------
 * 2. Pipe EOF -- close write end, reader gets 0
 * ---------------------------------------------------------------------- */

static void test_pipe_eof(void) {
    int fds[2];
    pipe(fds);

    /* Write some data first */
    write(fds[1], "data", 4);

    /* Close the write end */
    close(fds[1]);

    /* Read the data */
    char buf[16];
    ssize_t n1 = read(fds[0], buf, sizeof(buf));
    CHECK("pipe: read returns data before EOF", n1 == 4);

    /* Next read should return 0 (EOF) */
    ssize_t n2 = read(fds[0], buf, sizeof(buf));
    CHECK("pipe: read returns 0 after write-end closed", n2 == 0);

    close(fds[0]);
}

/* -----------------------------------------------------------------------
 * 3. SIGPIPE on write to closed read end
 *    Fork a child that writes to a pipe whose read end is closed.
 *    The child should get SIGPIPE (signal 13).
 * ---------------------------------------------------------------------- */

static void test_pipe_sigpipe(void) {
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: create its own pipe, close read end, write.
         * This avoids a race with the parent holding the read fd. */
        int pfd[2];
        pipe(pfd);
        close(pfd[0]); /* close read end -- no readers remain */

        /* Ensure default SIGPIPE handler (kills the process) */
        struct sigaction sa;
        sa.sa_handler = SIG_DFL;
        sa.sa_mask = 0;
        sa.sa_flags = 0;
        sa.sa_restorer = 0;
        sigaction(SIGPIPE, &sa, 0);

        /* Write to pipe with no readers -- should get SIGPIPE */
        write(pfd[1], "x", 1);

        /* If we get here, SIGPIPE was not delivered */
        _exit(99);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    CHECK("pipe: SIGPIPE on write to closed read end",
          WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE);
}

/* -----------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    printf("=== pipetest: pipe capacity/blocking tests ===\n");

    test_pipe_basic_rw();
    test_pipe_eof();
    test_pipe_sigpipe();

    printf("=== pipetest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
