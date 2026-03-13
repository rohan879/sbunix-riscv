#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int tests_passed = 0;
static int tests_failed = 0;

static void check(int cond, const char *name) {
    if (cond) {
        printf("[PASS] sigill: %s\n", name);
        tests_passed++;
    } else {
        printf("[FAIL] sigill: %s\n", name);
        tests_failed++;
    }
}

static void sigill_handler(int sig) {
    (void)sig;
    _exit(42);
}

static void trigger_illegal_instruction(void) {
    asm volatile(".2byte 0");
}

int main(void) {
    int status;
    pid_t pid;

    pid = fork();
    if (pid == 0) {
        struct sigaction sa;
        sa.sa_handler = sigill_handler;
        sa.sa_mask = 0;
        sa.sa_flags = 0;
        sa.sa_restorer = 0;
        sigaction(SIGILL, &sa, 0);
        trigger_illegal_instruction();
        _exit(1);
    }
    check(pid > 0, "fork for handler");
    waitpid(pid, &status, 0);
    check(WIFEXITED(status), "SIGILL handler exited normally");
    check(WEXITSTATUS(status) == 42, "SIGILL handler exit status");

    pid = fork();
    if (pid == 0) {
        trigger_illegal_instruction();
        _exit(1);
    }
    check(pid > 0, "fork for default");
    waitpid(pid, &status, 0);
    check(WIFSIGNALED(status), "SIGILL default terminated");
    check(WTERMSIG(status) == SIGILL, "SIGILL default signal number");

    printf("sigilltest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
