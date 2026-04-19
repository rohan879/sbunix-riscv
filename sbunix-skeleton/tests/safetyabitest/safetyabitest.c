#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <syscall.h>
#include <unistd.h>

static int passed;
static int failed;

static void check(const char *name, int ok) {
    if (ok) {
        printf("[PASS] safetyabi: %s\n", name);
        passed++;
    } else {
        printf("[FAIL] safetyabi: %s\n", name);
        failed++;
    }
}

static void test_wait4_bad_status_retry(void) {
    pid_t child = fork();
    if (child == 0)
        _exit(37);

    errno = 0;
    pid_t first = waitpid(child, (int *)0x40000000UL, 0);
    check("wait4_bad_status: first wait returns EFAULT",
          first == -1 && errno == EFAULT);

    int status = 0;
    pid_t second = waitpid(child, &status, 0);
    check("wait4_bad_status: child remains waitable",
          second == child && WIFEXITED(status) && WEXITSTATUS(status) == 37);
}

static void test_invalid_signal_handler_defaults(void) {
    pid_t child = fork();
    if (child == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = (void (*)(int))0x80000000UL;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);
        raise(SIGUSR1);
        _exit(99);
    }

    int status = 0;
    pid_t waited = waitpid(child, &status, 0);
    check("invalid_handler: child was reaped",
          waited == child);
    check("invalid_handler: default action terminates",
          WIFSIGNALED(status) && WTERMSIG(status) == SIGUSR1);
}

static void test_rt_sigsetsize_validation(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigset_t oldset = 0;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 0 };
    unsigned long bad_size = sizeof(sigset_t) + 8;

    long r = syscall(SYS_rt_sigprocmask, SIG_BLOCK, &set, &oldset, bad_size);
    check("sigsetsize: rt_sigprocmask rejects bad size", r == -EINVAL);

    r = syscall(SYS_rt_sigaction, SIGUSR1, NULL, NULL, bad_size);
    check("sigsetsize: rt_sigaction rejects bad size", r == -EINVAL);

    r = syscall(SYS_rt_sigpending, &oldset, bad_size);
    check("sigsetsize: rt_sigpending rejects bad size", r == -EINVAL);

    r = syscall(SYS_rt_sigsuspend, &set, bad_size);
    check("sigsetsize: rt_sigsuspend rejects bad size", r == -EINVAL);

    r = syscall(SYS_rt_sigtimedwait, &set, NULL, &ts, bad_size);
    check("sigsetsize: rt_sigtimedwait rejects bad size", r == -EINVAL);

    errno = 0;
    check("sigsetsize: libc sigprocmask still accepts native size",
          sigprocmask(SIG_BLOCK, &set, &oldset) == 0 && errno == 0);
    sigprocmask(SIG_SETMASK, &oldset, NULL);
}

int main(void) {
    printf("=== safetyabitest: safety ABI regression tests ===\n");

    test_wait4_bad_status_retry();
    test_invalid_signal_handler_defaults();
    test_rt_sigsetsize_validation();

    printf("=== safetyabitest: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
