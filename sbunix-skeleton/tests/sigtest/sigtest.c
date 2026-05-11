/*
 * sigtest — signal delivery and kill() test suite
 *
 * Exercises kill(pid, sig), signal(), SIG_IGN, SIGKILL, SIGSTOP/SIGCONT,
 * process-group kill, default termination, sigprocmask, and sigpending.
 *
 * Tests signal delivery and kill().
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <errno.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] sig: %s\n", label); g_pass++; } \
    else { printf("[FAIL] sig: %s\n", label); g_fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * Shared handler state
 * ---------------------------------------------------------------------- */

static volatile sig_atomic_t got_usr1 = 0;
static volatile sig_atomic_t got_usr2 = 0;

static void usr1_handler(int sig) { (void)sig; got_usr1 = 1; }
static void usr2_handler(int sig) { (void)sig; got_usr2 = 1; }

static int wait_for_byte(int fd, char want, int attempts) {
    for (int i = 0; i < attempts; i++) {
        char ch = 0;
        ssize_t n = read(fd, &ch, 1);
        if (n == 1)
            return ch == want;
        if (n == 0)
            return 0;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            return 0;
        usleep(10000);
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * 1. kill_self — kill(getpid(), SIGUSR1) with handler installed
 * ---------------------------------------------------------------------- */

static void test_kill_self(void) {
    got_usr1 = 0;
    signal(SIGUSR1, usr1_handler);
    int r = kill(getpid(), SIGUSR1);
    CHECK("kill_self: kill returns 0", r == 0);
    CHECK("kill_self: handler ran", got_usr1 == 1);
}

/* -----------------------------------------------------------------------
 * 2. kill_child — fork child, parent sends SIGUSR1, child receives it
 * ---------------------------------------------------------------------- */

static void test_kill_child(void) {
    int pfd[2];
    pipe(pfd);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: install handler, notify parent we are ready, then pause */
        close(pfd[0]);
        got_usr1 = 0;
        signal(SIGUSR1, usr1_handler);
        /* Signal parent that handler is installed */
        write(pfd[1], "R", 1);
        close(pfd[1]);
        /* Wait for signal delivery */
        while (!got_usr1)
            usleep(1000);
        _exit(got_usr1 ? 0 : 1);
    }

    /* Parent: wait for child to be ready, then send signal */
    close(pfd[1]);
    char buf[1];
    read(pfd[0], buf, 1);
    close(pfd[0]);

    kill(pid, SIGUSR1);

    int status;
    waitpid(pid, &status, 0);
    CHECK("kill_child: child received SIGUSR1",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* -----------------------------------------------------------------------
 * 3. kill_zero_check — kill(getpid(), 0) returns 0, no signal sent
 * ---------------------------------------------------------------------- */

static void test_kill_zero_check(void) {
    got_usr1 = 0;
    signal(SIGUSR1, usr1_handler);
    int r = kill(getpid(), 0);
    CHECK("kill_zero_check: returns 0", r == 0);
    CHECK("kill_zero_check: no handler invoked", got_usr1 == 0);
}

/* -----------------------------------------------------------------------
 * 4. kill_nonexistent — kill(99999, 0) returns -1, errno=ESRCH
 * ---------------------------------------------------------------------- */

static void test_kill_nonexistent(void) {
    errno = 0;
    int r = kill(99999, 0);
    CHECK("kill_nonexistent: returns -1", r == -1);
    CHECK("kill_nonexistent: errno == ESRCH", errno == ESRCH);
}

/* -----------------------------------------------------------------------
 * 5. kill_sigkill — fork child spinning, parent sends SIGKILL
 * ---------------------------------------------------------------------- */

static void test_kill_sigkill(void) {
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: spin forever; SIGKILL cannot be caught */
        signal(SIGKILL, usr1_handler);
        for (;;)
            usleep(1000);
        _exit(0);
    }

    usleep(50000); /* let child start running */
    kill(pid, SIGKILL);

    int status;
    waitpid(pid, &status, 0);
    CHECK("kill_sigkill: WIFSIGNALED", WIFSIGNALED(status));
    CHECK("kill_sigkill: WTERMSIG == SIGKILL", WTERMSIG(status) == SIGKILL);
}

/* -----------------------------------------------------------------------
 * 6. kill_sigstop_cont — fork child, SIGSTOP then SIGCONT, child resumes
 * ---------------------------------------------------------------------- */

static void test_kill_sigstop_cont(void) {
    int pfd[2];
    int status;

    if (pipe2(pfd, O_NONBLOCK) != 0) {
        CHECK("kill_sigstop_cont: pipe setup", 0);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        /* BF: Signal readiness only after the post-CONT handler is installed.
         * SIGCONT was removed as the child-release signal because its default
         * action can resume the process without running a handler; if it
         * arrives before pause() sleeps, the child can block forever. */
        got_usr1 = 0;
        signal(SIGUSR1, usr1_handler);
        write(pfd[1], "A", 1);
        while (!got_usr1)
            usleep(1000);
        write(pfd[1], "B", 1);
        close(pfd[1]);
        _exit(0);
    }
    if (pid < 0) {
        CHECK("kill_sigstop_cont: fork child", 0);
        close(pfd[0]);
        close(pfd[1]);
        return;
    }

    close(pfd[1]);

    /* BF: Keep the initial readiness handshake bounded too; otherwise a
     * failed child setup can pin the same CI path as a failed resume. */
    if (!wait_for_byte(pfd[0], 'A', 200)) {
        CHECK("kill_sigstop_cont: child ready", 0);
        kill(pid, SIGKILL);
        close(pfd[0]);
        waitpid(pid, &status, WNOHANG);
        return;
    }

    /* Stop the child */
    kill(pid, SIGSTOP);

    /* Wait until child is actually stopped */
    waitpid(pid, &status, WUNTRACED);
    int child_stopped = WIFSTOPPED(status);
    CHECK("kill_sigstop_cont: child stopped", child_stopped);

    /* Resume the child */
    if (child_stopped) {
        kill(pid, SIGCONT);
        /* Use SIGUSR1, not SIGCONT, as the deterministic post-resume wakeup. */
        kill(pid, SIGUSR1);
    }

    /* Child should now write "B" and exit */
    int wrote_after_cont = child_stopped && wait_for_byte(pfd[0], 'B', 200);
    close(pfd[0]);

    int resumed_and_exited = (wrote_after_cont &&
                              waitpid(pid, &status, 0) == pid &&
                              WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (!resumed_and_exited) {
        /* BF: Keep post-resume cleanup best-effort and nonblocking so this
         * signal regression reports failure rather than hanging CI. */
        kill(pid, SIGCONT);
        kill(pid, SIGKILL);
        waitpid(pid, &status, WNOHANG);
    }
    CHECK("kill_sigstop_cont: child resumed and exited", resumed_and_exited);
    CHECK("kill_sigstop_cont: child wrote after CONT", wrote_after_cont);
}

/* -----------------------------------------------------------------------
 * 7. kill_group — kill(-pgid, SIGUSR1) reaches all group members
 * ---------------------------------------------------------------------- */

static void test_killpg_current_group_self(void) {
    pid_t self = getpid();

    /* TEST: Exercise libc killpg(getpgrp(), sig) directly.  The process first
     * isolates itself so the group signal cannot reach the parent shell. */
    if (setpgid(0, self) != 0 && getpgrp() != self) {
        CHECK("killpg_current_group: isolated process group", 0);
        return;
    }
    CHECK("killpg_current_group: isolated process group", getpgrp() == self);

    got_usr1 = 0;
    signal(SIGUSR1, usr1_handler);
    int r = killpg(getpgrp(), SIGUSR1);
    CHECK("killpg_current_group: killpg returns 0", r == 0);
    CHECK("killpg_current_group: handler ran", got_usr1 == 1);
}

static void test_kill_group(void) {
    pid_t parent_pid = getpid();
    pid_t new_pgid = parent_pid;

    /* Put ourselves in our own process group */
    setpgid(0, new_pgid);

    int pfd1[2], pfd2[2];
    pipe(pfd1);
    pipe(pfd2);

    /* Fork two children in the same process group */
    pid_t c1 = fork();
    if (c1 == 0) {
        close(pfd1[0]); close(pfd2[0]); close(pfd2[1]);
        got_usr1 = 0;
        signal(SIGUSR1, usr1_handler);
        write(pfd1[1], "R", 1);
        close(pfd1[1]);
        while (!got_usr1)
            usleep(1000);
        _exit(0);
    }

    pid_t c2 = fork();
    if (c2 == 0) {
        close(pfd1[0]); close(pfd1[1]); close(pfd2[0]);
        got_usr1 = 0;
        signal(SIGUSR1, usr1_handler);
        write(pfd2[1], "R", 1);
        close(pfd2[1]);
        while (!got_usr1)
            usleep(1000);
        _exit(0);
    }

    close(pfd1[1]); close(pfd2[1]);

    /* Wait for both children to be ready */
    char buf[1];
    read(pfd1[0], buf, 1);
    read(pfd2[0], buf, 1);
    close(pfd1[0]); close(pfd2[0]);

    /* Install handler in parent too so we survive the group signal */
    got_usr1 = 0;
    signal(SIGUSR1, usr1_handler);

    /* Send SIGUSR1 to the entire process group */
    kill(-new_pgid, SIGUSR1);

    int s1, s2;
    waitpid(c1, &s1, 0);
    waitpid(c2, &s2, 0);

    CHECK("kill_group: child 1 received group signal",
          WIFEXITED(s1) && WEXITSTATUS(s1) == 0);
    CHECK("kill_group: child 2 received group signal",
          WIFEXITED(s2) && WEXITSTATUS(s2) == 0);
}

/* -----------------------------------------------------------------------
 * 8. signal_default_term — child sends itself SIGTERM (no handler),
 *    parent sees signal death
 * ---------------------------------------------------------------------- */

static void test_signal_default_term(void) {
    pid_t pid = fork();
    if (pid == 0) {
        /* Ensure default disposition */
        signal(SIGTERM, SIG_DFL);
        raise(SIGTERM);
        /* Should not reach here */
        _exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    CHECK("signal_default_term: WIFSIGNALED", WIFSIGNALED(status));
    CHECK("signal_default_term: WTERMSIG == SIGTERM",
          WTERMSIG(status) == SIGTERM);
}

/* -----------------------------------------------------------------------
 * 9. signal_ignore — signal(SIGUSR1, SIG_IGN), kill self, survive
 * ---------------------------------------------------------------------- */

static void test_signal_ignore(void) {
    signal(SIGUSR1, SIG_IGN);
    int r = kill(getpid(), SIGUSR1);
    CHECK("signal_ignore: kill returns 0", r == 0);
    /* If we are still alive, the signal was ignored */
    CHECK("signal_ignore: process survived", 1);

    /* Restore handler for later tests */
    signal(SIGUSR1, usr1_handler);
}

/* -----------------------------------------------------------------------
 * 10. signal_pending — block SIGUSR1, send it, sigpending shows pending
 * ---------------------------------------------------------------------- */

static void test_signal_pending(void) {
    sigset_t block_set, pending;

    /* Block SIGUSR1 */
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block_set, NULL);

    /* Install handler so delivery after unblock doesn't kill us */
    got_usr1 = 0;
    signal(SIGUSR1, usr1_handler);

    /* Send SIGUSR1 to self — should be queued, not delivered */
    kill(getpid(), SIGUSR1);
    CHECK("signal_pending: handler did NOT run while blocked", got_usr1 == 0);

    /* Check pending set */
    sigemptyset(&pending);
    sigpending(&pending);
    CHECK("signal_pending: SIGUSR1 is pending",
          sigismember(&pending, SIGUSR1) == 1);

    /* Unblock — signal should now be delivered */
    sigprocmask(SIG_UNBLOCK, &block_set, NULL);
    CHECK("signal_pending: handler ran after unblock", got_usr1 == 1);
}

/* -----------------------------------------------------------------------
 * 11. signal_mask — sigprocmask blocks signal, unblock delivers it
 * ---------------------------------------------------------------------- */

static void test_signal_mask(void) {
    sigset_t block_set, old_set;

    /* Install handler */
    got_usr2 = 0;
    signal(SIGUSR2, usr2_handler);

    /* Block SIGUSR2 */
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &block_set, &old_set);

    /* Send the signal — should not be delivered yet */
    kill(getpid(), SIGUSR2);
    CHECK("signal_mask: handler not called while blocked", got_usr2 == 0);

    /* Unblock — signal should now fire */
    sigprocmask(SIG_SETMASK, &old_set, NULL);
    CHECK("signal_mask: handler called after unblock", got_usr2 == 1);
}

/* -----------------------------------------------------------------------
 * 12. signal_mask_alias_oldset — set and oldset may alias the same object
 * ---------------------------------------------------------------------- */

static void test_signal_mask_alias_oldset(void) {
    sigset_t mask_and_old;
    sigset_t pending;

    got_usr1 = 0;
    signal(SIGUSR1, usr1_handler);

    sigemptyset(&mask_and_old);
    sigaddset(&mask_and_old, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask_and_old, &mask_and_old);

    kill(getpid(), SIGUSR1);
    CHECK("signal_mask_alias_oldset: handler not called while blocked",
          got_usr1 == 0);

    sigemptyset(&pending);
    sigpending(&pending);
    CHECK("signal_mask_alias_oldset: SIGUSR1 is pending",
          sigismember(&pending, SIGUSR1) == 1);

    sigprocmask(SIG_SETMASK, &mask_and_old, NULL);
    CHECK("signal_mask_alias_oldset: handler called after unblock",
          got_usr1 == 1);
}

/* -----------------------------------------------------------------------
 * 13. feedback_exact_block_then_self_send — exact evaluator scenario
 * ---------------------------------------------------------------------- */

static void test_feedback_exact_block_then_self_send(void) {
    sigset_t block_set, old_set;

    got_usr1 = 0;
    signal(SIGUSR1, usr1_handler);

    sigemptyset(&block_set);
    sigaddset(&block_set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block_set, &old_set);

    kill(getpid(), SIGUSR1);
    CHECK("feedback_exact_block_then_self_send: flag clear before unblock",
          got_usr1 == 0);

    sigprocmask(SIG_SETMASK, &old_set, NULL);
    CHECK("feedback_exact_block_then_self_send: flag set after unblock",
          got_usr1 == 1);
}

/* -----------------------------------------------------------------------
 * 14. sigaction_alias_oldact — act and oldact may alias the same object
 * ---------------------------------------------------------------------- */

static void test_sigaction_alias_oldact(void) {
    struct sigaction sa;

    got_usr1 = 0;
    signal(SIGUSR1, SIG_IGN);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = usr1_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, &sa);

    CHECK("sigaction_alias_oldact: old disposition returned",
          sa.sa_handler == SIG_IGN);

    kill(getpid(), SIGUSR1);
    CHECK("sigaction_alias_oldact: new handler installed", got_usr1 == 1);
}

/* -----------------------------------------------------------------------
 * 15. sigaltstack_alias_old — ss and old_ss may alias the same object
 * ---------------------------------------------------------------------- */

static void test_sigaltstack_alias_old(void) {
    stack_t ss;
    stack_t cur;
    void *expected_sp;
    size_t expected_size = SIGSTKSZ;

    expected_sp = malloc(expected_size);
    if (!expected_sp) {
        CHECK("sigaltstack_alias_old: allocated alt stack", 0);
        return;
    }

    ss.ss_sp = expected_sp;
    ss.ss_size = expected_size;
    ss.ss_flags = 0;

    CHECK("sigaltstack_alias_old: install succeeds",
          sigaltstack(&ss, &ss) == 0);
    CHECK("sigaltstack_alias_old: old state reported disabled",
          ss.ss_flags == SS_DISABLE);

    memset(&cur, 0, sizeof(cur));
    CHECK("sigaltstack_alias_old: query succeeds", sigaltstack(NULL, &cur) == 0);
    CHECK("sigaltstack_alias_old: new stack installed",
          cur.ss_sp == expected_sp && cur.ss_size == expected_size &&
          !(cur.ss_flags & SS_DISABLE));

    ss.ss_sp = NULL;
    ss.ss_size = 0;
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, NULL);
    free(expected_sp);
}

/* -----------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    printf("=== sigtest: signal delivery and kill() ===\n");

    test_kill_self();
    test_kill_child();
    test_kill_zero_check();
    test_kill_nonexistent();
    test_kill_sigkill();
    test_kill_sigstop_cont();
    test_killpg_current_group_self();
    test_kill_group();
    test_signal_default_term();
    test_signal_ignore();
    test_signal_pending();
    test_signal_mask();
    test_signal_mask_alias_oldset();
    test_feedback_exact_block_then_self_send();
    test_sigaction_alias_oldact();
    test_sigaltstack_alias_old();

    printf("=== sigtest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
