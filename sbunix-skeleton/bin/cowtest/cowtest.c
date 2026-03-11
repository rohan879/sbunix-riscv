#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

static void check(int cond, const char *name) {
    if (cond) { printf("[PASS] cow: %s\n", name); tests_passed++; }
    else { printf("[FAIL] cow: %s\n", name); tests_failed++; }
}

static int shared_data = 42;
static char big_buf[4096];

int main(void) {
    int status;
    pid_t pid;

    memset(big_buf, 'A', sizeof(big_buf));

    pid = fork();
    if (pid == 0) {
        if (shared_data != 42) exit(1);
        if (big_buf[0] != 'A') exit(2);
        shared_data = 99;
        big_buf[0] = 'B';
        exit(0);
    }
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child sees parent data");
    check(shared_data == 42, "parent data unchanged");
    check(big_buf[0] == 'A', "parent buffer unchanged");

    /* multi-fork */
    for (int i = 0; i < 5; i++) {
        pid = fork();
        if (pid == 0) { shared_data = i; exit(i); }
        waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == i, "multi-fork child ok");
    }
    check(shared_data == 42, "parent intact after multi-fork");

    printf("cowtest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
