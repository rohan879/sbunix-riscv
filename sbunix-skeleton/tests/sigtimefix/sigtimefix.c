#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <syscall.h>
#include <time.h>
#include <unistd.h>

long syscall(long num, ...);

static int g_pass;
static int g_fail;
static volatile sig_atomic_t got_usr1;
static volatile sig_atomic_t got_ucontext;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] sigtimefix: %s\n", label); g_pass++; } \
    else { printf("[FAIL] sigtimefix: %s\n", label); g_fail++; } \
} while (0)

static void usr1_handler(int sig) {
    (void)sig;
    got_usr1 = 1;
}

static void siginfo_handler(int sig, siginfo_t *info, void *uctx) {
    ucontext_t *uc = (ucontext_t *)uctx;
    if (sig == SIGUSR2 && info && info->si_signo == SIGUSR2 &&
        uc && uc->uc_mcontext.regs[2] != 0)
        got_ucontext = 1;
}

static void test_sigtimedwait_timeout(void) {
    sigset_t set;
    sigset_t old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, &old);

    struct timespec timeout = {0, 100000000};
    errno = 0;
    int ret = sigtimedwait(&set, NULL, &timeout);
    CHECK("sigtimedwait timeout returns EAGAIN",
          ret == -1 && errno == EAGAIN);

    sigprocmask(SIG_SETMASK, &old, NULL);
}

static void test_nanosleep_interrupt(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = usr1_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    pid_t parent = getpid();
    pid_t child = fork();
    if (child == 0) {
        struct timespec delay = {0, 100000000};
        nanosleep(&delay, NULL);
        kill(parent, SIGUSR1);
        _exit(0);
    }

    got_usr1 = 0;
    struct timespec req = {2, 0};
    struct timespec rem = {0, 0};
    errno = 0;
    int ret = nanosleep(&req, &rem);
    CHECK("nanosleep interrupted by signal",
          ret == -1 && errno == EINTR && got_usr1 == 1);
    CHECK("nanosleep reports remaining time",
          rem.tv_sec > 0 || rem.tv_nsec > 0);

    int status;
    waitpid(child, &status, 0);
}

static void test_sa_siginfo_ucontext(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = siginfo_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR2, &sa, NULL);

    got_ucontext = 0;
    raise(SIGUSR2);
    CHECK("SA_SIGINFO receives non-null ucontext", got_ucontext == 1);
}

static void permission_worker(void) {
    pid_t target = fork();
    if (target == 0) {
        setuid(2000);
        struct timespec delay = {0, 400000000};
        nanosleep(&delay, NULL);
        _exit(0);
    }

    setuid(1000);

    errno = 0;
    int kill_ret = kill(target, 0);
    int kill_ok = (kill_ret == -1 && errno == EPERM);

    siginfo_t info;
    memset(&info, 0, sizeof(info));
    long queue_ret = syscall(SYS_rt_sigqueueinfo, target, 0, &info);
    int queue_ok = (queue_ret == -EPERM);

    long tgkill_ret = syscall(SYS_tgkill, target, target, 0);
    int tgkill_ok = (tgkill_ret == -EPERM);

    int status;
    waitpid(target, &status, 0);
    _exit(kill_ok && queue_ok && tgkill_ok ? 0 : 1);
}

static void test_signal_permission_checks(void) {
    pid_t worker = fork();
    if (worker == 0)
        permission_worker();

    int status;
    waitpid(worker, &status, 0);
    CHECK("rt_sigqueueinfo and tgkill enforce UID permission",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_realtime_signal_ordering(void) {
    sigset_t rtset;
    sigset_t allrt;
    sigset_t old;
    union sigval value;
    siginfo_t info;

    sigemptyset(&rtset);
    sigaddset(&rtset, SIGRTMAX);
    sigprocmask(SIG_BLOCK, &rtset, &old);

    int queue_ok = 1;
    for (int i = 1; i <= 3; i++) {
        value.sival_int = 100 + i;
        if (sigqueue(getpid(), SIGRTMAX, value) != 0)
            queue_ok = 0;
    }
    CHECK("realtime FIFO sigqueue returns 0", queue_ok);

    int fifo_ok = 1;
    for (int i = 1; i <= 3; i++) {
        memset(&info, 0, sizeof(info));
        int ret = sigtimedwait(&rtset, &info, NULL);
        if (ret != SIGRTMAX || info.si_value.sival_int != 100 + i)
            fifo_ok = 0;
    }
    CHECK("realtime FIFO sigtimedwait preserves queued order", fifo_ok);

    sigemptyset(&allrt);
    for (int sig = SIGRTMIN; sig <= SIGRTMAX; sig++) {
        sigaddset(&allrt, sig);
        sigprocmask(SIG_BLOCK, &allrt, NULL);
    }
    queue_ok = 1;
    for (int sig = SIGRTMAX; sig >= SIGRTMIN; sig--) {
        value.sival_int = sig;
        if (sigqueue(getpid(), sig, value) != 0)
            queue_ok = 0;
    }
    CHECK("realtime lowest sigqueue returns 0", queue_ok);
    memset(&info, 0, sizeof(info));
    int ret = sigwaitinfo(&allrt, &info);
    CHECK("realtime sigwaitinfo returns lowest pending signal",
          ret == SIGRTMIN && info.si_signo == SIGRTMIN);
    for (int sig = SIGRTMIN + 1; sig <= SIGRTMAX; sig++) {
        memset(&info, 0, sizeof(info));
        (void)sigtimedwait(&allrt, &info, NULL);
    }

    sigprocmask(SIG_SETMASK, &old, NULL);
}

int main(void) {
    printf("=== sigtimefix: signal/time regression tests ===\n");
    test_sigtimedwait_timeout();
    test_nanosleep_interrupt();
    test_sa_siginfo_ucontext();
    test_signal_permission_checks();
    test_realtime_signal_ordering();
    printf("=== sigtimefix: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
