#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>

static int tests_passed = 0;
static int tests_failed = 0;

static void check(int cond, const char *name) {
    if (cond) { printf("[PASS] sighand: %s\n", name); tests_passed++; }
    else { printf("[FAIL] sighand: %s\n", name); tests_failed++; }
}

static volatile int handler_sig = 0;
static void usr1_handler(int sig) { handler_sig = sig; }
static void usr2_handler(int sig) { handler_sig = sig; }

int main(void) {
    struct sigaction sa, old;

    /* sigaction installs handler */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = usr1_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    int r = sigaction(SIGUSR1, &sa, NULL);
    check(r == 0, "sigaction install");

    handler_sig = 0;
    raise(SIGUSR1);
    check(handler_sig == SIGUSR1, "handler got SIGUSR1");

    /* sigaction retrieves old */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = usr2_handler;
    r = sigaction(SIGUSR1, &sa, &old);
    check(r == 0, "sigaction replace");
    check(old.sa_handler == usr1_handler, "old handler correct");

    /* SIG_IGN */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGUSR2, &sa, NULL);
    handler_sig = 0;
    raise(SIGUSR2);
    check(handler_sig == 0, "SIG_IGN works");

    /* SIG_DFL terminates */
    pid_t pid = fork();
    if (pid == 0) {
        sa.sa_handler = SIG_DFL;
        sigaction(SIGUSR1, &sa, NULL);
        raise(SIGUSR1);
        exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    check(WIFSIGNALED(status), "SIG_DFL terminates");

    printf("sighandtest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
