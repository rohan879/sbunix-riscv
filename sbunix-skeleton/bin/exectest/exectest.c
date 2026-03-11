#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>

static int tests_passed = 0;
static int tests_failed = 0;

static void check(int cond, const char *name) {
    if (cond) { printf("[PASS] exec: %s\n", name); tests_passed++; }
    else { printf("[FAIL] exec: %s\n", name); tests_failed++; }
}

int main(void) {
    int status;
    pid_t pid;

    /* execve echo */
    pid = fork();
    if (pid == 0) {
        char *argv[] = {"/bin/echo", "exec_ok", NULL};
        char *envp[] = {NULL};
        execve("/bin/echo", argv, envp);
        exit(127);
    }
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "execve echo");

    /* execve nonexistent */
    pid = fork();
    if (pid == 0) {
        char *argv[] = {"/bin/nonexistent", NULL};
        char *envp[] = {NULL};
        int r = execve("/bin/nonexistent", argv, envp);
        exit(r == -1 ? 0 : 1);
    }
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "execve nonexistent fails");

    /* /bin/sh -c */
    pid = fork();
    if (pid == 0) {
        char *argv[] = {"/bin/sh", "-c", "echo sh_ok", NULL};
        char *envp[] = {NULL};
        execve("/bin/sh", argv, envp);
        exit(127);
    }
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "/bin/sh -c works");

    /* execve with multiple args */
    pid = fork();
    if (pid == 0) {
        char *argv[] = {"/bin/echo", "a", "b", "c", NULL};
        char *envp[] = {NULL};
        execve("/bin/echo", argv, envp);
        exit(127);
    }
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "execve multi-arg");

    printf("exectest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
