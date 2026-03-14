/*
 * libctest.c -- Additional libc function tests for SBUnix.
 *
 * Exercises:
 *   1.  strlen returns correct size_t values
 *   2.  strtol overflow detection (ERANGE)
 *   3.  strtoul overflow detection (ERANGE)
 *   4.  malloc/free/double-free safety
 *   5.  getopt basic parsing
 *   6.  popen/pclose basic functionality
 *   7.  fdopen
 *   8.  remove()
 *   9.  abs(INT_MIN) doesn't crash
 *  10.  snprintf %s with precision
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/wait.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else      { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * 1. strlen
 * ---------------------------------------------------------------------- */

static void test_strlen(void) {
    CHECK("libc: strlen empty", strlen("") == 0);
    CHECK("libc: strlen hello", strlen("hello") == 5);
    CHECK("libc: strlen 1-char", strlen("x") == 1);
}

/* -----------------------------------------------------------------------
 * 2. strtol overflow
 * ---------------------------------------------------------------------- */

static void test_strtol_overflow(void) {
    char *end;

    /* Positive overflow */
    errno = 0;
    long v1 = strtol("99999999999999999999", &end, 10);
    CHECK("libc: strtol positive overflow returns LONG_MAX", v1 == LONG_MAX);
    CHECK("libc: strtol positive overflow sets ERANGE", errno == ERANGE);

    /* Negative overflow */
    errno = 0;
    long v2 = strtol("-99999999999999999999", &end, 10);
    CHECK("libc: strtol negative overflow returns LONG_MIN", v2 == LONG_MIN);
    CHECK("libc: strtol negative overflow sets ERANGE", errno == ERANGE);

    /* Valid conversion */
    errno = 0;
    long v3 = strtol("12345", &end, 10);
    CHECK("libc: strtol valid conversion", v3 == 12345 && *end == '\0');
}

/* -----------------------------------------------------------------------
 * 3. strtoul overflow
 * ---------------------------------------------------------------------- */

static void test_strtoul_overflow(void) {
    char *end;

    errno = 0;
    unsigned long v = strtoul("99999999999999999999999", &end, 10);
    CHECK("libc: strtoul overflow returns ULONG_MAX", v == ULONG_MAX);
    CHECK("libc: strtoul overflow sets ERANGE", errno == ERANGE);

    /* Valid conversion */
    errno = 0;
    unsigned long v2 = strtoul("54321", &end, 10);
    CHECK("libc: strtoul valid conversion", v2 == 54321UL && *end == '\0');
}

/* -----------------------------------------------------------------------
 * 4. malloc/free/double-free safety
 * ---------------------------------------------------------------------- */

static void test_malloc_free(void) {
    /* Basic allocation */
    void *p = malloc(128);
    CHECK("libc: malloc returns non-NULL", p != (void *)0);

    if (p) {
        /* Write and read back */
        memset(p, 0xAB, 128);
        CHECK("libc: malloc memory writable",
              ((unsigned char *)p)[0] == 0xAB &&
              ((unsigned char *)p)[127] == 0xAB);

        free(p);

        /* Double-free: should not crash (our allocator guards against this) */
        free(p);
        CHECK("libc: double-free does not crash", 1);
    }

    /* malloc(0) should return NULL per our implementation */
    void *z = malloc(0);
    CHECK("libc: malloc(0) returns NULL", z == (void *)0);
}

/* -----------------------------------------------------------------------
 * 5. getopt basic parsing
 * ---------------------------------------------------------------------- */

static void test_getopt(void) {
    /* Reset getopt state */
    optind = 1;
    opterr = 0; /* suppress error messages */

    char *argv[] = { "prog", "-a", "-b", "arg", "extra", (void *)0 };
    int argc = 5;
    int got_a = 0, got_b = 0;
    char *b_arg = (void *)0;

    int c;
    while ((c = getopt(argc, argv, "ab:")) != -1) {
        if (c == 'a') got_a = 1;
        if (c == 'b') { got_b = 1; b_arg = optarg; }
    }

    CHECK("libc: getopt finds -a", got_a);
    CHECK("libc: getopt finds -b with arg",
          got_b && b_arg && strcmp(b_arg, "arg") == 0);
    CHECK("libc: getopt optind points past options", optind == 4);

    /* Reset for any future callers */
    optind = 1;
}

/* -----------------------------------------------------------------------
 * 6. popen/pclose
 * ---------------------------------------------------------------------- */

static void test_popen(void) {
    FILE *fp = popen("/bin/echo popen_works", "r");
    CHECK("libc: popen returns non-NULL", fp != (void *)0);

    if (fp) {
        char buf[64];
        memset(buf, 0, sizeof(buf));
        char *r = fgets(buf, sizeof(buf), fp);
        int status = pclose(fp);

        CHECK("libc: popen read output",
              r != (void *)0 && strncmp(buf, "popen_works", 11) == 0);
        CHECK("libc: pclose returns status", WIFEXITED(status));
    }
}

/* -----------------------------------------------------------------------
 * 7. fdopen
 * ---------------------------------------------------------------------- */

static void test_fdopen(void) {
    /* Create a temp file, write via fd, read via FILE* */
    const char *path = "/tmp/fdopen_test";
    unlink(path);

    int fd = open(path, O_CREAT | O_RDWR, 0644);
    CHECK("libc: fdopen: open temp file", fd >= 0);
    if (fd < 0) return;

    write(fd, "fdopen_ok", 9);
    lseek(fd, 0, SEEK_SET);

    FILE *fp = fdopen(fd, "r");
    CHECK("libc: fdopen returns non-NULL", fp != (void *)0);

    if (fp) {
        char buf[32];
        memset(buf, 0, sizeof(buf));
        char *r = fgets(buf, sizeof(buf), fp);
        CHECK("libc: fdopen read matches",
              r != (void *)0 && strcmp(buf, "fdopen_ok") == 0);
        fclose(fp); /* also closes fd */
    } else {
        close(fd);
    }

    unlink(path);
}

/* -----------------------------------------------------------------------
 * 8. remove()
 * ---------------------------------------------------------------------- */

static void test_remove(void) {
    const char *path = "/tmp/remove_test";
    unlink(path);

    /* Create a file */
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    CHECK("libc: remove: create file", fd >= 0);
    if (fd >= 0) close(fd);

    /* Verify it exists */
    int r = access(path, F_OK);
    CHECK("libc: remove: file exists before remove", r == 0);

    /* Remove it */
    r = remove(path);
    CHECK("libc: remove returns 0", r == 0);

    /* Verify it is gone */
    r = access(path, F_OK);
    CHECK("libc: remove: file gone after remove", r == -1);
}

/* -----------------------------------------------------------------------
 * 9. abs(INT_MIN) doesn't crash
 * ---------------------------------------------------------------------- */

static void test_abs_intmin(void) {
    /* abs(INT_MIN) is technically UB in C, but our implementation
     * casts through unsigned to avoid it.  Just verify no crash. */
    int v = abs(INT_MIN);
    /* The result is implementation-defined; we just check it runs */
    (void)v;
    CHECK("libc: abs(INT_MIN) does not crash", 1);

    /* Normal cases */
    CHECK("libc: abs(-42) == 42", abs(-42) == 42);
    CHECK("libc: abs(0) == 0", abs(0) == 0);
    CHECK("libc: abs(99) == 99", abs(99) == 99);
}

/* -----------------------------------------------------------------------
 * 10. snprintf %s with precision
 * ---------------------------------------------------------------------- */

static void test_snprintf_precision(void) {
    char buf[64];

    /* Truncation via buffer size */
    int n = snprintf(buf, 8, "hello %s", "world");
    CHECK("libc: snprintf truncates to size",
          n == 11 && strcmp(buf, "hello w") == 0);

    /* %s with small buffer */
    memset(buf, 'X', sizeof(buf));
    n = snprintf(buf, 4, "%s", "abcdef");
    CHECK("libc: snprintf %s truncated",
          n == 6 && strcmp(buf, "abc") == 0);

    /* Exact fit */
    memset(buf, 0, sizeof(buf));
    n = snprintf(buf, 64, "%s", "exact");
    CHECK("libc: snprintf %s exact fit",
          n == 5 && strcmp(buf, "exact") == 0);
}

/* -----------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    printf("=== libctest: additional libc tests ===\n");

    test_strlen();
    test_strtol_overflow();
    test_strtoul_overflow();
    test_malloc_free();
    test_getopt();
    test_popen();
    test_fdopen();
    test_remove();
    test_abs_intmin();
    test_snprintf_precision();

    printf("=== libctest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
