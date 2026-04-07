/*
 * sighandtest — signal handlers and sigaction test suite
 *
 * Exercises sigaction(), SA_RESETHAND, SA_NODEFER, SA_RESTART,
 * nested signal delivery, handler argument correctness,
 * register restoration after sigreturn, and SIGKILL/SIGSTOP rejection.
 *
 * Tests signal handlers and sigaction().
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <errno.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] sighand: %s\n", label); g_pass++; } \
    else { printf("[FAIL] sighand: %s\n", label); g_fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * Handler state
 * ---------------------------------------------------------------------- */

static volatile sig_atomic_t handler_ran = 0;
static volatile sig_atomic_t handler_signum = 0;
static volatile sig_atomic_t nested_usr2 = 0;
static volatile sig_atomic_t resethand_count = 0;
static volatile sig_atomic_t nodefer_reentrant = 0;
static volatile sig_atomic_t restart_handler_ran = 0;

/* -----------------------------------------------------------------------
 * 1. sigaction_basic — install handler via sigaction, send signal
 * ---------------------------------------------------------------------- */

static void basic_handler(int sig) {
    (void)sig;
    handler_ran = 1;
}

static void test_sigaction_basic(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = basic_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    handler_ran = 0;
    int r = sigaction(SIGUSR1, &sa, NULL);
    CHECK("sigaction_basic: sigaction returns 0", r == 0);

    raise(SIGUSR1);
    CHECK("sigaction_basic: handler ran", handler_ran == 1);
}

/* -----------------------------------------------------------------------
 * 2. sigaction_oldact — sigaction returns previous handler in oldact
 * ---------------------------------------------------------------------- */

static void handler_a(int sig) { (void)sig; }
static void handler_b(int sig) { (void)sig; }

static void test_sigaction_oldact(void) {
    struct sigaction sa, old;

    /* Install handler_a */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_a;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    /* Replace with handler_b, retrieve old */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_b;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    memset(&old, 0, sizeof(old));
    int r = sigaction(SIGUSR1, &sa, &old);
    CHECK("sigaction_oldact: returns 0", r == 0);
    CHECK("sigaction_oldact: old handler is handler_a",
          old.sa_handler == handler_a);
}

/* -----------------------------------------------------------------------
 * 3. sigaction_mask — sa_mask blocks additional signals during handler
 * ---------------------------------------------------------------------- */

static volatile sig_atomic_t mask_usr2_during = 0;

static void mask_usr1_handler(int sig) {
    (void)sig;
    /*
     * SIGUSR2 should be blocked during this handler (added to sa_mask).
     * Send SIGUSR2 to self — if blocked, mask_usr2_during stays 0.
     */
    mask_usr2_during = 0;
    kill(getpid(), SIGUSR2);
    /* If SIGUSR2 was delivered synchronously, mask_usr2_during would be 1.
     * With proper masking, it should still be 0 here. */
    if (mask_usr2_during == 0)
        handler_ran = 1; /* sa_mask worked */
    else
        handler_ran = 0;
}

static void mask_usr2_handler(int sig) {
    (void)sig;
    mask_usr2_during = 1;
}

static void test_sigaction_mask(void) {
    struct sigaction sa;

    /* Install SIGUSR2 handler */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = mask_usr2_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR2, &sa, NULL);

    /* Install SIGUSR1 handler with SIGUSR2 in sa_mask */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = mask_usr1_handler;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR2);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    handler_ran = 0;
    mask_usr2_during = 0;
    raise(SIGUSR1);

    CHECK("sigaction_mask: sa_mask blocked SIGUSR2 during handler",
          handler_ran == 1);
    /* After handler returns, SIGUSR2 should have been delivered */
    CHECK("sigaction_mask: SIGUSR2 delivered after handler",
          mask_usr2_during == 1);
}

/* -----------------------------------------------------------------------
 * 4. sa_resethand — first signal calls handler, second uses SIG_DFL
 * ---------------------------------------------------------------------- */

static void resethand_handler(int sig) {
    (void)sig;
    resethand_count++;
}

static void test_sa_resethand(void) {
    /* Run in child so SIG_DFL termination doesn't kill the test process */
    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = resethand_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESETHAND;
        sigaction(SIGUSR1, &sa, NULL);

        resethand_count = 0;

        /* First signal: handler runs */
        raise(SIGUSR1);
        if (resethand_count != 1)
            _exit(1);

        /* Second signal: disposition should be SIG_DFL now => terminates */
        raise(SIGUSR1);

        /* Should not reach here */
        _exit(2);
    }

    int status;
    waitpid(pid, &status, 0);
    CHECK("sa_resethand: first signal called handler, second killed",
          WIFSIGNALED(status) && WTERMSIG(status) == SIGUSR1);
}

/* -----------------------------------------------------------------------
 * 5. sa_nodefer — signal is NOT blocked during handler
 * ---------------------------------------------------------------------- */

static volatile sig_atomic_t nodefer_depth = 0;

static void nodefer_handler(int sig) {
    (void)sig;
    nodefer_depth++;
    if (nodefer_depth == 1) {
        /* Re-raise the same signal; with SA_NODEFER it should be delivered
         * immediately (re-entrant) */
        raise(SIGUSR1);
    }
}

static void test_sa_nodefer(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = nodefer_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGUSR1, &sa, NULL);

    nodefer_depth = 0;
    raise(SIGUSR1);

    CHECK("sa_nodefer: handler was re-entered", nodefer_depth >= 2);
}

/* -----------------------------------------------------------------------
 * 6. sa_restart — SA_RESTART causes interrupted read to resume
 * ---------------------------------------------------------------------- */

static void restart_handler(int sig) {
    (void)sig;
    restart_handler_ran = 1;
}

static void test_sa_restart(void) {
    int pfd[2];
    pipe(pfd);

    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[1]);

        /* Install SIGUSR1 handler with SA_RESTART */
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = restart_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGUSR1, &sa, NULL);

        restart_handler_ran = 0;

        /* Blocking read — will be interrupted by signal, should restart */
        char buf[16];
        ssize_t n = read(pfd[0], buf, sizeof(buf));
        close(pfd[0]);

        /* Should have received the data after restart */
        if (n == 4 && memcmp(buf, "data", 4) == 0 && restart_handler_ran)
            _exit(0);
        else
            _exit(1);
    }

    close(pfd[0]);

    /* Give child time to enter read() */
    usleep(50000);

    /* Send signal to interrupt the read */
    kill(pid, SIGUSR1);

    /* Give handler time to run */
    usleep(50000);

    /* Now write the data — read should have restarted and receive it */
    write(pfd[1], "data", 4);
    close(pfd[1]);

    int status;
    waitpid(pid, &status, 0);
    CHECK("sa_restart: interrupted read resumed and got data",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* -----------------------------------------------------------------------
 * 7. sigreturn_restores_regs — registers preserved across signal handler
 *
 * We set a local variable before the signal, then verify it is unchanged
 * after the handler returns. This exercises sigreturn restoring the
 * user-mode register state.
 * ---------------------------------------------------------------------- */

static volatile sig_atomic_t reg_handler_ran = 0;

static void reg_handler(int sig) {
    (void)sig;
    reg_handler_ran = 1;
}

static void test_sigreturn_restores_regs(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reg_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    /*
     * Use volatile locals to defeat optimisation. The compiler must keep
     * these in registers or on stack; sigreturn must restore them.
     */
    volatile long a0 = 0xDEADBEEF;
    volatile long a1 = 0xCAFEBABE;

    reg_handler_ran = 0;
    raise(SIGUSR1);

    CHECK("sigreturn_restores_regs: handler ran", reg_handler_ran == 1);
    CHECK("sigreturn_restores_regs: a0 preserved",
          a0 == (long)0xDEADBEEF);
    CHECK("sigreturn_restores_regs: a1 preserved",
          a1 == (long)0xCAFEBABE);
}

/* -----------------------------------------------------------------------
 * 8. nested_signals — while handling SIGUSR1, receive SIGUSR2
 * ---------------------------------------------------------------------- */

static void nested_usr1_handler(int sig) {
    (void)sig;
    handler_ran = 1;
    /* SIGUSR2 is NOT in sa_mask, so it should be deliverable */
    raise(SIGUSR2);
}

static void nested_usr2_handler(int sig) {
    (void)sig;
    nested_usr2 = 1;
}

static void test_nested_signals(void) {
    struct sigaction sa;

    /* Install SIGUSR2 handler */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = nested_usr2_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR2, &sa, NULL);

    /* Install SIGUSR1 handler — do NOT mask SIGUSR2 */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = nested_usr1_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    handler_ran = 0;
    nested_usr2 = 0;
    raise(SIGUSR1);

    CHECK("nested_signals: SIGUSR1 handler ran", handler_ran == 1);
    CHECK("nested_signals: SIGUSR2 delivered during SIGUSR1 handler",
          nested_usr2 == 1);
}

/* -----------------------------------------------------------------------
 * 9. handler_gets_signum — handler receives correct signal number
 * ---------------------------------------------------------------------- */

static void signum_handler(int sig) {
    handler_signum = sig;
}

static void test_handler_gets_signum(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signum_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    /* Test with SIGUSR1 */
    handler_signum = 0;
    sigaction(SIGUSR1, &sa, NULL);
    raise(SIGUSR1);
    CHECK("handler_gets_signum: SIGUSR1 arg correct",
          handler_signum == SIGUSR1);

    /* Test with SIGUSR2 */
    handler_signum = 0;
    sigaction(SIGUSR2, &sa, NULL);
    raise(SIGUSR2);
    CHECK("handler_gets_signum: SIGUSR2 arg correct",
          handler_signum == SIGUSR2);
}

/* -----------------------------------------------------------------------
 * 10. sigaction_sigkill_fails — cannot install handler for SIGKILL
 * ---------------------------------------------------------------------- */

static void test_sigaction_sigkill_fails(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = basic_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    errno = 0;
    int r = sigaction(SIGKILL, &sa, NULL);
    CHECK("sigaction_sigkill_fails: returns -1", r == -1);
    CHECK("sigaction_sigkill_fails: errno == EINVAL", errno == EINVAL);
}

/* -----------------------------------------------------------------------
 * 11. sigaction_sigstop_fails — cannot install handler for SIGSTOP
 * ---------------------------------------------------------------------- */

static void test_sigaction_sigstop_fails(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = basic_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    errno = 0;
    int r = sigaction(SIGSTOP, &sa, NULL);
    CHECK("sigaction_sigstop_fails: returns -1", r == -1);
    CHECK("sigaction_sigstop_fails: errno == EINVAL", errno == EINVAL);
}

/* -----------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    printf("=== sighandtest: signal handlers and sigaction ===\n");

    test_sigaction_basic();
    test_sigaction_oldact();
    test_sigaction_mask();
    test_sa_resethand();
    test_sa_nodefer();
    test_sa_restart();
    test_sigreturn_restores_regs();
    test_nested_signals();
    test_handler_gets_signum();
    test_sigaction_sigkill_fails();
    test_sigaction_sigstop_fails();

    printf("=== sighandtest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
