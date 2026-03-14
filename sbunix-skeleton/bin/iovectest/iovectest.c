/*
 * iovectest — readv/writev test
 *
 * Tests scatter/gather I/O with multiple iovec buffers through
 * a pipe.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * 1. writev_readv_pipe — writev to pipe, readv back, verify data
 * ---------------------------------------------------------------------- */

static void test_writev_readv_pipe(void)
{
    int pfd[2];
    int r = pipe(pfd);
    CHECK("pipe: created", r == 0);
    if (r != 0) return;

    /* Prepare 3 write buffers */
    char w1[] = "Hello";
    char w2[] = " ";
    char w3[] = "World";

    struct iovec wv[3];
    wv[0].iov_base = w1;
    wv[0].iov_len  = 5;
    wv[1].iov_base = w2;
    wv[1].iov_len  = 1;
    wv[2].iov_base = w3;
    wv[2].iov_len  = 5;

    ssize_t nw = writev(pfd[1], wv, 3);
    CHECK("writev: wrote 11 bytes", nw == 11);

    /* Prepare 3 read buffers */
    char r1[8], r2[4], r3[8];
    memset(r1, 0, sizeof(r1));
    memset(r2, 0, sizeof(r2));
    memset(r3, 0, sizeof(r3));

    struct iovec rv[3];
    rv[0].iov_base = r1;
    rv[0].iov_len  = 5;
    rv[1].iov_base = r2;
    rv[1].iov_len  = 1;
    rv[2].iov_base = r3;
    rv[2].iov_len  = 5;

    ssize_t nr = readv(pfd[0], rv, 3);
    CHECK("readv: read 11 bytes", nr == 11);

    CHECK("readv: buf1 is 'Hello'", memcmp(r1, "Hello", 5) == 0);
    CHECK("readv: buf2 is ' '",     memcmp(r2, " ", 1) == 0);
    CHECK("readv: buf3 is 'World'", memcmp(r3, "World", 5) == 0);

    close(pfd[0]);
    close(pfd[1]);
}

/* -----------------------------------------------------------------------
 * 2. writev_single — writev with 1 iovec (degenerate case)
 * ---------------------------------------------------------------------- */

static void test_writev_single(void)
{
    int pfd[2];
    int r = pipe(pfd);
    CHECK("single_pipe: created", r == 0);
    if (r != 0) return;

    char data[] = "test";
    struct iovec iov;
    iov.iov_base = data;
    iov.iov_len  = 4;

    ssize_t nw = writev(pfd[1], &iov, 1);
    CHECK("writev_single: wrote 4 bytes", nw == 4);

    char buf[8];
    memset(buf, 0, sizeof(buf));
    struct iovec riov;
    riov.iov_base = buf;
    riov.iov_len  = 4;

    ssize_t nr = readv(pfd[0], &riov, 1);
    CHECK("readv_single: read 4 bytes", nr == 4);
    CHECK("readv_single: data is 'test'", memcmp(buf, "test", 4) == 0);

    close(pfd[0]);
    close(pfd[1]);
}

/* --- Main --- */

int main(void)
{
    printf("=== iovectest: readv/writev ===\n");

    test_writev_readv_pipe();
    test_writev_single();

    printf("=== iovectest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
