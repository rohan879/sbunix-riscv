/*
 * polltest — ppoll test
 *
 * Tests ppoll on a pipe: timeout with no data, and readability
 * after writing data.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * 1. timeout — ppoll on empty pipe with short timeout, verify returns 0
 * ---------------------------------------------------------------------- */

static void test_timeout(void)
{
    int pfd[2];
    int r = pipe(pfd);
    CHECK("timeout_pipe: created", r == 0);
    if (r != 0) return;

    struct pollfd fds;
    fds.fd = pfd[0];
    fds.events = POLLIN;
    fds.revents = 0;

    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 10000000; /* 10 ms */

    int ret = ppoll(&fds, 1, &ts, 0);
    CHECK("timeout: ppoll returns 0 (timed out)", ret == 0);
    CHECK("timeout: no revents set", fds.revents == 0);

    close(pfd[0]);
    close(pfd[1]);
}

/* -----------------------------------------------------------------------
 * 2. readable — write to pipe, ppoll reports POLLIN
 * ---------------------------------------------------------------------- */

static void test_readable(void)
{
    int pfd[2];
    int r = pipe(pfd);
    CHECK("readable_pipe: created", r == 0);
    if (r != 0) return;

    /* Write data to pipe first */
    write(pfd[1], "data", 4);

    struct pollfd fds;
    fds.fd = pfd[0];
    fds.events = POLLIN;
    fds.revents = 0;

    struct timespec ts;
    ts.tv_sec = 1;
    ts.tv_nsec = 0;

    int ret = ppoll(&fds, 1, &ts, 0);
    CHECK("readable: ppoll returns 1", ret == 1);
    CHECK("readable: POLLIN set", (fds.revents & POLLIN) != 0);

    /* Drain the data */
    char buf[8];
    read(pfd[0], buf, sizeof(buf));

    close(pfd[0]);
    close(pfd[1]);
}

/* -----------------------------------------------------------------------
 * 3. write_ready — ppoll on write end of pipe reports POLLOUT
 * ---------------------------------------------------------------------- */

static void test_write_ready(void)
{
    int pfd[2];
    int r = pipe(pfd);
    CHECK("write_pipe: created", r == 0);
    if (r != 0) return;

    struct pollfd fds;
    fds.fd = pfd[1];
    fds.events = POLLOUT;
    fds.revents = 0;

    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 10000000; /* 10 ms */

    int ret = ppoll(&fds, 1, &ts, 0);
    CHECK("write_ready: ppoll returns >= 1", ret >= 1);
    CHECK("write_ready: POLLOUT set", (fds.revents & POLLOUT) != 0);

    close(pfd[0]);
    close(pfd[1]);
}

/* --- Main --- */

int main(void)
{
    printf("=== polltest: ppoll ===\n");

    test_timeout();
    test_readable();
    test_write_ready();

    printf("=== polltest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
