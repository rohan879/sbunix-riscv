#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <time.h>

static int tests_passed = 0;
static int tests_failed = 0;

static void check(int cond, const char *name) {
    if (cond) { printf("[PASS] clock: %s\n", name); tests_passed++; }
    else { printf("[FAIL] clock: %s\n", name); tests_failed++; }
}

int main(void) {
    struct timespec rt1, rt2, mt1, mt2;

    /* --- CLOCK_REALTIME basic checks --- */
    int r = clock_gettime(CLOCK_REALTIME, &rt1);
    check(r == 0, "CLOCK_REALTIME gettime ok");
    check(rt1.tv_sec > 1577836800, "CLOCK_REALTIME time after 2020");
    check(rt1.tv_nsec >= 0 && rt1.tv_nsec < 1000000000,
          "CLOCK_REALTIME nsec in valid range");

    /* --- CLOCK_MONOTONIC basic checks --- */
    r = clock_gettime(CLOCK_MONOTONIC, &mt1);
    check(r == 0, "CLOCK_MONOTONIC gettime ok");
    check(mt1.tv_sec >= 0, "CLOCK_MONOTONIC sec >= 0");
    check(mt1.tv_nsec >= 0 && mt1.tv_nsec < 1000000000,
          "CLOCK_MONOTONIC nsec in valid range");

    /* --- Sleep and verify both clocks advance --- */
    struct timespec slp = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100ms */
    nanosleep(&slp, NULL);

    clock_gettime(CLOCK_REALTIME, &rt2);
    clock_gettime(CLOCK_MONOTONIC, &mt2);

    long long rt_diff = (rt2.tv_sec - rt1.tv_sec) * 1000000000LL +
                        (rt2.tv_nsec - rt1.tv_nsec);
    long long mt_diff = (mt2.tv_sec - mt1.tv_sec) * 1000000000LL +
                        (mt2.tv_nsec - mt1.tv_nsec);

    check(rt_diff > 0, "CLOCK_REALTIME advances after nanosleep");
    check(rt_diff >= 50000000, "CLOCK_REALTIME elapsed >= 50ms");
    check(mt_diff > 0, "CLOCK_MONOTONIC advances after nanosleep");
    check(mt_diff >= 50000000, "CLOCK_MONOTONIC elapsed >= 50ms");

    /* --- Time doesn't go backwards --- */
    check(rt2.tv_sec > rt1.tv_sec ||
          (rt2.tv_sec == rt1.tv_sec && rt2.tv_nsec >= rt1.tv_nsec),
          "CLOCK_REALTIME does not go backwards");
    check(mt2.tv_sec > mt1.tv_sec ||
          (mt2.tv_sec == mt1.tv_sec && mt2.tv_nsec >= mt1.tv_nsec),
          "CLOCK_MONOTONIC does not go backwards");

    /* --- nsec fields valid after sleep --- */
    check(rt2.tv_nsec >= 0 && rt2.tv_nsec < 1000000000,
          "CLOCK_REALTIME nsec valid after sleep");
    check(mt2.tv_nsec >= 0 && mt2.tv_nsec < 1000000000,
          "CLOCK_MONOTONIC nsec valid after sleep");

    /* --- time() returns same epoch as CLOCK_REALTIME --- */
    time_t t = time(NULL);
    struct timespec rt3;
    clock_gettime(CLOCK_REALTIME, &rt3);
    check(t > 1577836800, "time() returns time after 2020");
    /* time() and clock_gettime should agree within 2 seconds */
    long long tdiff = (long long)rt3.tv_sec - (long long)t;
    if (tdiff < 0) tdiff = -tdiff;
    check(tdiff <= 2, "time() agrees with CLOCK_REALTIME");

    printf("clocktest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
