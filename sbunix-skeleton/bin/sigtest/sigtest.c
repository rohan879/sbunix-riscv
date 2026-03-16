#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static int tests_passed = 0;
static int tests_failed = 0;

static void check(int cond, const char *name) {
    if (cond) { printf("[PASS] sig: %s\n", name); tests_passed++; }
    else { printf("[FAIL] sig: %s\n", name); tests_failed++; }
}

static volatile int got_sig = 0;
static void handler(int sig) { (void)sig; got_sig = 1; }

int main(void) {
    int status;
    pid_t pid;

    /* kill self with SIGUSR1 */
    got_sig = 0;
    signal(SIGUSR1, handler);
    kill(getpid(), SIGUSR1);
    check(got_sig == 1, "kill self SIGUSR1");

    /* kill child with SIGTERM */
    pid = fork();
    if (pid == 0) { sleep(10); exit(0); }
    check(pid > 0, "fork for kill");
    usleep(50000);
    kill(pid, SIGTERM);
    waitpid(pid, &status, 0);
    check(WIFSIGNALED(status), "child signaled");
    check(WTERMSIG(status) == SIGTERM, "killed by SIGTERM");

    /* SIGKILL cannot be caught */
    pid = fork();
    if (pid == 0) { signal(SIGKILL, handler); sleep(10); exit(0); }
    usleep(50000);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    check(WIFSIGNALED(status), "SIGKILL terminates");
    check(WTERMSIG(status) == SIGKILL, "killed by SIGKILL");

    printf("sigtest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
