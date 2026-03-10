/*
 * sigpipetest.c — signals and pipes test suite
 *
 * Tests signal delivery (kill, sigaction, SIG_IGN, SIGSEGV),
 * pipe I/O (basic, EOF, fork, large transfer), dup2 redirection,
 * and wait-status inspection macros (WIFEXITED, WIFSIGNALED, WTERMSIG).
 */

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* --- Handlers for signal tests --- */

static volatile sig_atomic_t usr1_flag = 0;
static void usr1_handler(int sig) { (void)sig; usr1_flag = 1; }

static void segv_handler(int sig) { (void)sig; _exit(42); }

/* --- Signal tests --- */

static void test_kill_sigint(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        for (;;); /* spin forever, will be killed */
        _exit(1);
    }
    /* Brief delay to ensure child is running */
    for (volatile int i = 0; i < 1000000; i++);
    kill(pid, SIGINT);
    int status;
    waitpid(pid, &status, 0);
    CHECK("kill_sigint: WIFSIGNALED",  WIFSIGNALED(status));
    CHECK("kill_sigint: WTERMSIG==2",  WTERMSIG(status) == 2);
}

static void test_kill_sigkill(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        for (;;); /* spin forever, will be killed */
        _exit(1);
    }
    for (volatile int i = 0; i < 1000000; i++);
    kill(pid, 9);
    int status;
    waitpid(pid, &status, 0);
    CHECK("kill_sigkill: WIFSIGNALED", WIFSIGNALED(status));
    CHECK("kill_sigkill: WTERMSIG==9", WTERMSIG(status) == 9);
}

static void test_sigaction_handler(void)
{
    struct sigaction sa;
    sa.sa_handler = usr1_handler;
    sa.sa_mask = 0;
    sa.sa_flags = 0;
    sa.sa_restorer = 0;

    usr1_flag = 0;
    sigaction(SIGUSR1, &sa, 0);
    raise(SIGUSR1);
    CHECK("sigaction_handler: flag set", usr1_flag == 1);
}

static void test_sig_ign(void)
{
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sa.sa_mask = 0;
    sa.sa_flags = 0;
    sa.sa_restorer = 0;
    sigaction(SIGUSR1, &sa, 0);

    raise(SIGUSR1);
    /* If we get here, the signal was ignored */
    CHECK("sig_ign: process survived", 1);

    /* Restore default handler */
    sa.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &sa, 0);
}

static void test_sigsegv_handler(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction sa;
        sa.sa_handler = segv_handler;
        sa.sa_mask = 0;
        sa.sa_flags = 0;
        sa.sa_restorer = 0;
        sigaction(SIGSEGV, &sa, 0);
        *(volatile int *)0 = 0;
        _exit(1); /* should not reach here */
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK("sigsegv_handler: WIFEXITED",       WIFEXITED(status));
    CHECK("sigsegv_handler: WEXITSTATUS==42", WEXITSTATUS(status) == 42);
}

static void test_sigsegv_default(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        /* No handler installed — default action is to kill with SIGSEGV */
        *(volatile int *)0 = 0;
        _exit(1); /* should not reach here */
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK("sigsegv_default: WIFSIGNALED",  WIFSIGNALED(status));
    CHECK("sigsegv_default: WTERMSIG==11", WTERMSIG(status) == 11);
}

/* --- Pipe tests --- */

static void test_pipe_basic(void)
{
    int pfd[2];
    pipe(pfd);
    write(pfd[1], "hello", 5);
    char buf[16];
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(pfd[0], buf, sizeof(buf));
    close(pfd[0]);
    close(pfd[1]);
    CHECK("pipe_basic: read 5 bytes",  n == 5);
    CHECK("pipe_basic: data matches",  memcmp(buf, "hello", 5) == 0);
}

static void test_pipe_eof(void)
{
    int pfd[2];
    pipe(pfd);
    close(pfd[1]); /* close write end */
    char buf[16];
    ssize_t n = read(pfd[0], buf, sizeof(buf));
    close(pfd[0]);
    CHECK("pipe_eof: read returns 0", n == 0);
}

static void test_pipe_fork(void)
{
    int pfd[2];
    pipe(pfd);
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: read from pipe */
        close(pfd[1]);
        char buf[16];
        memset(buf, 0, sizeof(buf));
        ssize_t n = read(pfd[0], buf, sizeof(buf));
        close(pfd[0]);
        if (n == 4 && memcmp(buf, "data", 4) == 0)
            _exit(0);
        _exit(1);
    }
    /* Parent: write to pipe */
    close(pfd[0]);
    write(pfd[1], "data", 4);
    close(pfd[1]);
    int status;
    waitpid(pid, &status, 0);
    CHECK("pipe_fork: child got data", WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_pipe_large(void)
{
    int pfd[2];
    pipe(pfd);

    /* Prepare 4000-byte pattern: each byte = index % 256 */
    char wbuf[4000];
    for (int i = 0; i < 4000; i++)
        wbuf[i] = (char)(i % 256);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: read all data and verify */
        close(pfd[1]);
        char rbuf[4000];
        int total = 0;
        while (total < 4000) {
            ssize_t n = read(pfd[0], rbuf + total, 4000 - total);
            if (n <= 0)
                break;
            total += (int)n;
        }
        close(pfd[0]);
        /* Verify data integrity */
        if (total == 4000 && memcmp(rbuf, wbuf, 4000) == 0)
            _exit(0);
        _exit(1);
    }
    /* Parent: write all data */
    close(pfd[0]);
    int written = 0;
    while (written < 4000) {
        ssize_t n = write(pfd[1], wbuf + written, 4000 - written);
        if (n <= 0)
            break;
        written += (int)n;
    }
    close(pfd[1]);
    int status;
    waitpid(pid, &status, 0);
    CHECK("pipe_large: child received 4000 bytes",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_dup2_redirect(void)
{
    int pfd[2];
    pipe(pfd);
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: redirect stdout to pipe write end */
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        write(STDOUT_FILENO, "hi", 2);
        _exit(0);
    }
    /* Parent: read from pipe read end */
    close(pfd[1]);
    char buf[16];
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(pfd[0], buf, sizeof(buf));
    close(pfd[0]);
    int status;
    waitpid(pid, &status, 0);
    CHECK("dup2_redirect: read 2 bytes",  n == 2);
    CHECK("dup2_redirect: data is 'hi'",  memcmp(buf, "hi", 2) == 0);
}

/* --- Wait status tests --- */

static void test_wifsignaled_wtermsig(void)
{
    /* First: child that exits normally */
    pid_t pid1 = fork();
    if (pid1 == 0)
        _exit(0);
    int status1;
    waitpid(pid1, &status1, 0);
    CHECK("wifsignaled_wtermsig: normal exit WIFEXITED",  WIFEXITED(status1));
    CHECK("wifsignaled_wtermsig: normal exit !WIFSIGNALED", !WIFSIGNALED(status1));

    /* Second: child killed by SIGINT */
    pid_t pid2 = fork();
    if (pid2 == 0) {
        for (;;);
        _exit(1);
    }
    for (volatile int i = 0; i < 1000000; i++);
    kill(pid2, SIGINT);
    int status2;
    waitpid(pid2, &status2, 0);
    CHECK("wifsignaled_wtermsig: killed WIFSIGNALED",  WIFSIGNALED(status2));
    CHECK("wifsignaled_wtermsig: killed WTERMSIG==2",  WTERMSIG(status2) == 2);
}

/* --- Main --- */

int main(void)
{
    printf("=== sigpipetest: signals and pipes ===\n");

    /* Signal tests */
    test_kill_sigint();
    test_kill_sigkill();
    test_sigaction_handler();
    test_sig_ign();
    test_sigsegv_handler();
    test_sigsegv_default();

    /* Pipe tests */
    test_pipe_basic();
    test_pipe_eof();
    test_pipe_fork();
    test_pipe_large();

    /* Dup2 test */
    test_dup2_redirect();

    /* Wait status tests */
    test_wifsignaled_wtermsig();

    printf("=== sigpipetest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
