#include <stdio.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <signal.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

static volatile sig_atomic_t alarm_flag = 0;

static void alarm_handler(int sig)
{
    (void)sig;
    alarm_flag = 1;
}

static void test_time_epoch(void)
{
    time_t t = time(NULL);
    CHECK("time_epoch", t > 1000000000);
}

static void test_time_monotonic(void)
{
    time_t t1 = time(NULL);
    time_t t2 = time(NULL);
    CHECK("time_monotonic", t2 >= t1);
}

static void test_sleep_returns(void)
{
    time_t before = time(NULL);
    sleep(1);
    time_t after = time(NULL);
    CHECK("sleep_returns", after - before >= 1 && after - before <= 5);
}

static void test_preempt_two_children(void)
{
    int pipe0[2], pipe1[2];
    pipe(pipe0);
    pipe(pipe1);

    pid_t p0 = fork();
    if (p0 == 0) {
        close(pipe0[0]);
        close(pipe1[0]);
        close(pipe1[1]);
        for (volatile int i = 0; i < 5000000; i++);
        write(pipe0[1], "A", 1);
        close(pipe0[1]);
        _exit(0);
    }

    pid_t p1 = fork();
    if (p1 == 0) {
        close(pipe0[0]);
        close(pipe0[1]);
        close(pipe1[0]);
        for (volatile int i = 0; i < 5000000; i++);
        write(pipe1[1], "B", 1);
        close(pipe1[1]);
        _exit(0);
    }

    close(pipe0[1]);
    close(pipe1[1]);

    char buf0, buf1;
    int r0 = read(pipe0[0], &buf0, 1);
    int r1 = read(pipe1[0], &buf1, 1);
    close(pipe0[0]);
    close(pipe1[0]);

    int s0, s1;
    waitpid(p0, &s0, 0);
    waitpid(p1, &s1, 0);

    CHECK("preempt_two_children",
          r0 == 1 && r1 == 1 &&
          WIFEXITED(s0) && WEXITSTATUS(s0) == 0 &&
          WIFEXITED(s1) && WEXITSTATUS(s1) == 0);
}

static void test_preempt_three_children(void)
{
    int pipes[3][2];
    pid_t pids[3];
    int i;

    for (i = 0; i < 3; i++)
        pipe(pipes[i]);

    for (i = 0; i < 3; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            int j;
            for (j = 0; j < 3; j++) {
                close(pipes[j][0]);
                if (j != i)
                    close(pipes[j][1]);
            }
            for (volatile int k = 0; k < 5000000; k++);
            write(pipes[i][1], "X", 1);
            close(pipes[i][1]);
            _exit(0);
        }
    }

    for (i = 0; i < 3; i++)
        close(pipes[i][1]);

    int ok = 1;
    for (i = 0; i < 3; i++) {
        char buf;
        if (read(pipes[i][0], &buf, 1) != 1)
            ok = 0;
        close(pipes[i][0]);
    }

    for (i = 0; i < 3; i++) {
        int s;
        waitpid(pids[i], &s, 0);
        if (!WIFEXITED(s) || WEXITSTATUS(s) != 0)
            ok = 0;
    }

    CHECK("preempt_three_children", ok);
}

static void test_preempt_parent_child(void)
{
    int cpipe[2];
    pipe(cpipe);

    pid_t pid = fork();
    if (pid == 0) {
        close(cpipe[0]);
        for (volatile int i = 0; i < 5000000; i++);
        write(cpipe[1], "C", 1);
        close(cpipe[1]);
        _exit(0);
    }

    close(cpipe[1]);

    /* Parent also does a spin loop while child runs concurrently */
    for (volatile int i = 0; i < 5000000; i++);

    /* Read child's byte from child pipe */
    char cbuf;
    int rc = read(cpipe[0], &cbuf, 1);
    close(cpipe[0]);

    int s;
    waitpid(pid, &s, 0);

    /* Both parent and child completed their spin loops */
    CHECK("preempt_parent_child",
          rc == 1 && cbuf == 'C' &&
          WIFEXITED(s) && WEXITSTATUS(s) == 0);
}

static void test_alarm_signal(void)
{
    struct sigaction sa;
    sa.sa_handler = alarm_handler;
    sa.sa_mask = 0;
    sa.sa_flags = 0;
    sa.sa_restorer = 0;
    sigaction(SIGALRM, &sa, 0);

    alarm_flag = 0;
    alarm(1);
    for (volatile int i = 0; i < 50000000 && !alarm_flag; i++);
    CHECK("alarm_signal", alarm_flag == 1);
}

int main(void)
{
    printf("=== schedtest: preemptive multi-tasking ===\n");

    test_time_epoch();
    test_time_monotonic();
    test_sleep_returns();
    test_preempt_two_children();
    test_preempt_three_children();
    test_preempt_parent_child();
    test_alarm_signal();

    printf("=== schedtest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
