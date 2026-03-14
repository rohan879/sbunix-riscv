/*
 * wnohangtest — waitpid WNOHANG test
 *
 * Tests non-blocking waitpid returns 0 while child is running
 * and returns child pid after it exits.
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * 1. wnohang_running — WNOHANG returns 0 while child is still alive
 * ---------------------------------------------------------------------- */

static void test_wnohang_running(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: sleep to stay alive while parent checks */
        sleep(2);
        _exit(7);
    }

    /* Parent: immediately try WNOHANG — child is still sleeping */
    int status = -1;
    pid_t ret = waitpid(pid, &status, WNOHANG);
    CHECK("wnohang_running: returns 0 (child still alive)", ret == 0);

    /* Now wait for the child to actually exit */
    status = 0;
    ret = waitpid(pid, &status, 0);
    CHECK("wnohang_running: blocking wait returns child pid", ret == pid);
    CHECK("wnohang_running: WIFEXITED", WIFEXITED(status));
    CHECK("wnohang_running: WEXITSTATUS == 7",
          WIFEXITED(status) && WEXITSTATUS(status) == 7);
}

/* -----------------------------------------------------------------------
 * 2. wnohang_exited — WNOHANG returns pid after child exits
 * ---------------------------------------------------------------------- */

static void test_wnohang_exited(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        _exit(42);
    }

    /* Give the child time to exit */
    sleep(1);

    int status = 0;
    pid_t ret = waitpid(pid, &status, WNOHANG);
    /*
     * After the delay the child should have exited.
     * WNOHANG should return the child pid.
     */
    CHECK("wnohang_exited: returns child pid", ret == pid);
    CHECK("wnohang_exited: WIFEXITED", WIFEXITED(status));
    CHECK("wnohang_exited: WEXITSTATUS == 42",
          WIFEXITED(status) && WEXITSTATUS(status) == 42);
}

/* --- Main --- */

int main(void)
{
    printf("=== wnohangtest: waitpid WNOHANG ===\n");

    test_wnohang_running();
    test_wnohang_exited();

    printf("=== wnohangtest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
