#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

static int tests_passed = 0;
static int tests_failed = 0;

static void check(int cond, const char *name) {
    if (cond) { printf("[PASS] pgrp: %s\n", name); tests_passed++; }
    else { printf("[FAIL] pgrp: %s\n", name); tests_failed++; }
}

int main(void) {
    pid_t mypid = getpid();
    int status;
    pid_t pid;

    pid_t pgrp = getpgrp();
    check(pgrp > 0, "getpgrp positive");

    int r = setpgid(0, mypid);
    check(r == 0, "setpgid self");
    check(getpgrp() == mypid, "getpgrp matches");

    /* child inherits pgid */
    pid = fork();
    if (pid == 0) { exit(getpgrp() == mypid ? 0 : 1); }
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child inherits pgid");

    /* setpgid on child */
    pid = fork();
    if (pid == 0) { sleep(1); exit(0); }
    r = setpgid(pid, pid);
    check(r == 0, "setpgid child");
    check(getpgid(pid) == pid, "child pgid changed");
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "setpgid child exited cleanly");

    /* setsid */
    pid = fork();
    if (pid == 0) {
        pid_t sid = setsid();
        exit(sid == getpid() ? 0 : 1);
    }
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "setsid works");

    pid_t sid = getsid(0);
    check(sid > 0, "getsid positive");

    errno = 0;
    check(getpgid(9999) == -1 && errno == ESRCH, "getpgid bad pid");

    errno = 0;
    check(getsid(9999) == -1 && errno == ESRCH, "getsid bad pid");

    errno = 0;
    check(setsid() == -1 && errno == EPERM, "setsid fails for pgrp leader");

    printf("pgrptest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
