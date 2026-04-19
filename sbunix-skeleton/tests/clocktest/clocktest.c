/*
 * clocktest -- wall-clock time tests
 *
 * Exercises clock_gettime (CLOCK_REALTIME, CLOCK_MONOTONIC),
 * gettimeofday, time(), and verifies clocks advance and don't go backward.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <time.h>
#include <sys/time.h>
#include <syscall.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] clock: %s\n", label); g_pass++; } \
    else      { printf("[FAIL] clock: %s\n", label); g_fail++; } \
} while (0)

/* Epoch timestamp for Jan 1 2020 00:00:00 UTC */
#define EPOCH_2020 1577836800L
#define EPOCH_2100 4102444800L

static int sec_in_expected_range(long sec) {
    return sec >= EPOCH_2020 && sec < EPOCH_2100;
}

/* -----------------------------------------------------------------------
 * 1. clock_realtime_valid: clock_gettime(CLOCK_REALTIME) returns
 *    time > 1577836800 (Jan 2020)
 * ----------------------------------------------------------------------- */
static void test_clock_realtime_valid(void) {
    struct timespec ts;
    int r = clock_gettime(CLOCK_REALTIME, &ts);
    CHECK("clock_realtime_valid",
          r == 0 && sec_in_expected_range(ts.tv_sec));
}

/* -----------------------------------------------------------------------
 * 2. clock_realtime_nsec_range: nanoseconds field in [0, 999999999]
 * ----------------------------------------------------------------------- */
static void test_clock_realtime_nsec_range(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    CHECK("clock_realtime_nsec_range",
          ts.tv_nsec >= 0 && ts.tv_nsec <= 999999999);
}

/* -----------------------------------------------------------------------
 * 3. clock_monotonic_valid: clock_gettime(CLOCK_MONOTONIC) returns >= 0
 * ----------------------------------------------------------------------- */
static void test_clock_monotonic_valid(void) {
    struct timespec ts;
    int r = clock_gettime(CLOCK_MONOTONIC, &ts);
    CHECK("clock_monotonic_valid",
          r == 0 && ts.tv_sec >= 0);
}

/* -----------------------------------------------------------------------
 * 4. clock_monotonic_nsec_range: nanoseconds field in [0, 999999999]
 * ----------------------------------------------------------------------- */
static void test_clock_monotonic_nsec_range(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    CHECK("clock_monotonic_nsec_range",
          ts.tv_nsec >= 0 && ts.tv_nsec <= 999999999);
}

/* -----------------------------------------------------------------------
 * Helper: compare two timespecs. Returns 1 if a < b.
 * ----------------------------------------------------------------------- */
static int ts_less(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec < b->tv_sec) return 1;
    if (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec) return 1;
    return 0;
}

/* -----------------------------------------------------------------------
 * 5. clock_realtime_advances: Two calls to CLOCK_REALTIME after
 *    nanosleep(100ms), second > first
 * ----------------------------------------------------------------------- */
static void test_clock_realtime_advances(void) {
    struct timespec t1, t2;
    clock_gettime(CLOCK_REALTIME, &t1);

    struct timespec slp = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100ms */
    nanosleep(&slp, NULL);

    clock_gettime(CLOCK_REALTIME, &t2);
    CHECK("clock_realtime_advances", ts_less(&t1, &t2));
}

/* -----------------------------------------------------------------------
 * 6. clock_monotonic_advances: Same for CLOCK_MONOTONIC
 * ----------------------------------------------------------------------- */
static void test_clock_monotonic_advances(void) {
    struct timespec t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);

    struct timespec slp = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100ms */
    nanosleep(&slp, NULL);

    clock_gettime(CLOCK_MONOTONIC, &t2);
    CHECK("clock_monotonic_advances", ts_less(&t1, &t2));
}

/* -----------------------------------------------------------------------
 * 7. clock_monotonic_no_backward: Call 100 times in loop, never goes backward
 * ----------------------------------------------------------------------- */
static void test_clock_monotonic_no_backward(void) {
    struct timespec prev, cur;
    clock_gettime(CLOCK_MONOTONIC, &prev);

    int ok = 1;
    for (int i = 0; i < 100; i++) {
        clock_gettime(CLOCK_MONOTONIC, &cur);
        /* cur must be >= prev (not strictly less) */
        if (ts_less(&cur, &prev)) {
            ok = 0;
            break;
        }
        prev = cur;
    }
    CHECK("clock_monotonic_no_backward", ok);
}

static void test_clock_realtime_no_backward(void) {
    struct timespec prev, cur;
    clock_gettime(CLOCK_REALTIME, &prev);

    int ok = 1;
    for (int i = 0; i < 100; i++) {
        clock_gettime(CLOCK_REALTIME, &cur);
        if (ts_less(&cur, &prev)) {
            ok = 0;
            break;
        }
        prev = cur;
    }
    CHECK("clock_realtime_no_backward", ok);
}

/* -----------------------------------------------------------------------
 * 8. gettimeofday_valid: gettimeofday returns time > 1577836800
 * ----------------------------------------------------------------------- */
static void test_gettimeofday_valid(void) {
    struct timeval tv;
    int r = gettimeofday(&tv, NULL);
    CHECK("gettimeofday_valid",
          r == 0 && sec_in_expected_range(tv.tv_sec));
}

static void test_raw_gettimeofday_valid(void) {
    struct timeval tv;
    long r = syscall(SYS_gettimeofday, &tv, NULL);
    CHECK("raw_gettimeofday_valid",
          r == 0 && sec_in_expected_range(tv.tv_sec) &&
          tv.tv_usec >= 0 && tv.tv_usec < 1000000);
}

/* -----------------------------------------------------------------------
 * 9. gettimeofday_matches_clockgettime: Both return same second
 *    (within 1s tolerance)
 * ----------------------------------------------------------------------- */
static void test_gettimeofday_matches_clockgettime(void) {
    struct timeval tv;
    struct timespec ts;

    gettimeofday(&tv, NULL);
    clock_gettime(CLOCK_REALTIME, &ts);

    long long diff = (long long)ts.tv_sec - (long long)tv.tv_sec;
    if (diff < 0) diff = -diff;
    CHECK("gettimeofday_matches_clockgettime", diff <= 1);
}

/* -----------------------------------------------------------------------
 * 10. time_returns_epoch: time(NULL) returns seconds since epoch > 1577836800
 * ----------------------------------------------------------------------- */
static void test_time_returns_epoch(void) {
    time_t t = time(NULL);
    CHECK("time_returns_epoch", sec_in_expected_range(t));
}

/* ----------------------------------------------------------------------- */

int main(void) {
    printf("=== clocktest: wall-clock time ===\n");

    test_clock_realtime_valid();
    test_clock_realtime_nsec_range();
    test_clock_monotonic_valid();
    test_clock_monotonic_nsec_range();
    test_clock_realtime_advances();
    test_clock_monotonic_advances();
    test_clock_monotonic_no_backward();
    test_clock_realtime_no_backward();
    test_gettimeofday_valid();
    test_raw_gettimeofday_valid();
    test_gettimeofday_matches_clockgettime();
    test_time_returns_epoch();

    printf("=== clocktest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
