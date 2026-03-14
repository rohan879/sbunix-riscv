#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

static int tests_passed = 0;
static int tests_failed = 0;

static void check(int cond, const char *name) {
    if (cond) { printf("[PASS] clock: %s\n", name); tests_passed++; }
    else { printf("[FAIL] clock: %s\n", name); tests_failed++; }
}

int main(void) {
    struct timespec ts1, ts2;

    int r = clock_gettime(CLOCK_REALTIME, &ts1);
    check(r == 0, "clock_gettime ok");
    check(ts1.tv_sec > 1577836800, "time after 2020");
    check(ts1.tv_nsec >= 0 && ts1.tv_nsec < 1000000000, "nsec valid");

    usleep(100000);
    clock_gettime(CLOCK_REALTIME, &ts2);
    long long diff = (ts2.tv_sec - ts1.tv_sec) * 1000000000LL +
                     (ts2.tv_nsec - ts1.tv_nsec);
    check(diff > 0, "time advances");
    check(diff >= 50000000, "elapsed >= 50ms");

    struct timespec ts3;
    clock_gettime(CLOCK_REALTIME, &ts3);
    check(ts3.tv_sec > 0, "second sample positive");
    check(ts3.tv_sec > ts1.tv_sec ||
          (ts3.tv_sec == ts1.tv_sec && ts3.tv_nsec >= ts1.tv_nsec),
          "samples do not move backwards");

    printf("clocktest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
