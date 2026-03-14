/*
 * sigmasktest — sigprocmask test
 *
 * Tests signal blocking/unblocking with sigprocmask and verifies
 * that SIGKILL cannot be blocked.
 */

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

static volatile sig_atomic_t g_usr1_count = 0;

static void usr1_handler(int sig)
{
    (void)sig;
    g_usr1_count++;
}

/* -----------------------------------------------------------------------
 * 1. block_unblock — block SIGUSR1, send, verify delayed delivery
 * ---------------------------------------------------------------------- */

static void test_block_unblock(void)
{
    /* Install handler for SIGUSR1 */
    struct sigaction sa;
    sa.sa_handler = usr1_handler;
    sa.sa_mask = 0;
    sa.sa_flags = 0;
    sa.sa_restorer = 0;
    sigaction(SIGUSR1, &sa, 0);

    g_usr1_count = 0;

    /* Block SIGUSR1 */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, 0);

    /* Send SIGUSR1 to self — should be pending, not delivered */
    raise(SIGUSR1);
    CHECK("block: handler NOT called while blocked", g_usr1_count == 0);

    /* Unblock SIGUSR1 — pending signal should now be delivered */
    sigprocmask(SIG_UNBLOCK, &mask, 0);
    CHECK("unblock: handler called after unblock", g_usr1_count == 1);

    /* Restore default handler */
    sa.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &sa, 0);
}

/* -----------------------------------------------------------------------
 * 2. sigkill_unblockable — SIGKILL cannot be blocked
 * ---------------------------------------------------------------------- */

static void test_sigkill_unblockable(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: try to block SIGKILL, then sleep forever */
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGKILL);
        sigprocmask(SIG_BLOCK, &mask, 0);

        /* Sleep indefinitely; parent will SIGKILL us */
        sleep(10);

        /* Should not reach here */
        _exit(0);
    }

    /* Give the child time to set up its signal mask */
    sleep(1);

    /* Parent sends SIGKILL — should not be blockable */
    kill(pid, SIGKILL);

    int status;
    waitpid(pid, &status, 0);
    CHECK("sigkill: child was killed by signal", WIFSIGNALED(status));
    CHECK("sigkill: WTERMSIG == 9", WTERMSIG(status) == 9);
}

/* -----------------------------------------------------------------------
 * 3. oldset — sigprocmask returns previous mask in oldset
 * ---------------------------------------------------------------------- */

static void test_oldset(void)
{
    sigset_t mask, oldmask;

    /* Get current mask */
    sigemptyset(&mask);
    sigprocmask(SIG_BLOCK, 0, &oldmask);

    /* Block SIGUSR2 */
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    sigset_t prev;
    sigprocmask(SIG_BLOCK, &mask, &prev);

    /* prev should be the old mask (without SIGUSR2) */
    CHECK("oldset: SIGUSR2 not in previous mask",
          !sigismember(&prev, SIGUSR2));

    /* Now query again — SIGUSR2 should be in current mask */
    sigset_t cur;
    sigprocmask(SIG_BLOCK, 0, &cur);
    CHECK("oldset: SIGUSR2 now in current mask",
          sigismember(&cur, SIGUSR2));

    /* Restore original mask */
    sigprocmask(SIG_SETMASK, &oldmask, 0);
}

/* --- Main --- */

int main(void)
{
    printf("=== sigmasktest: sigprocmask ===\n");

    test_block_unblock();
    test_sigkill_unblockable();
    test_oldset();

    printf("=== sigmasktest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
