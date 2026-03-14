/*
 * ftrunctest — ftruncate tests on ext2 filesystem
 *
 * Exercises shrinking, zeroing, extending, and error cases.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * 1. shrink — write data, ftruncate to smaller size, verify
 * ---------------------------------------------------------------------- */

static void test_shrink(void)
{
    const char *path = "/mnt/ftrunc_shrink";
    unlink(path);

    int fd = open(path, O_CREAT | O_RDWR, 0644);
    CHECK("shrink: open", fd >= 0);
    if (fd < 0) return;

    write(fd, "hello world!", 12);

    int r = ftruncate(fd, 5);
    CHECK("shrink: ftruncate to 5", r == 0);
    close(fd);

    struct stat st;
    r = stat(path, &st);
    CHECK("shrink: stat returns 0", r == 0);
    CHECK("shrink: st_size == 5", st.st_size == 5);

    /* Verify contents */
    fd = open(path, O_RDONLY);
    CHECK("shrink: reopen", fd >= 0);
    if (fd >= 0) {
        char buf[16];
        memset(buf, 0, sizeof(buf));
        ssize_t n = read(fd, buf, sizeof(buf));
        CHECK("shrink: read 5 bytes", n == 5);
        CHECK("shrink: data is 'hello'", memcmp(buf, "hello", 5) == 0);
        close(fd);
    }

    unlink(path);
}

/* -----------------------------------------------------------------------
 * 2. to_zero — ftruncate to 0, verify empty
 * ---------------------------------------------------------------------- */

static void test_to_zero(void)
{
    const char *path = "/mnt/ftrunc_zero";
    unlink(path);

    int fd = open(path, O_CREAT | O_RDWR, 0644);
    CHECK("to_zero: open", fd >= 0);
    if (fd < 0) return;

    write(fd, "some data here", 14);

    int r = ftruncate(fd, 0);
    CHECK("to_zero: ftruncate to 0", r == 0);
    close(fd);

    struct stat st;
    r = stat(path, &st);
    CHECK("to_zero: stat returns 0", r == 0);
    CHECK("to_zero: st_size == 0", st.st_size == 0);

    unlink(path);
}

/* -----------------------------------------------------------------------
 * 3. extend — ftruncate to extend, verify zeros
 * ---------------------------------------------------------------------- */

static void test_extend(void)
{
    const char *path = "/mnt/ftrunc_ext";
    unlink(path);

    int fd = open(path, O_CREAT | O_RDWR, 0644);
    CHECK("extend: open", fd >= 0);
    if (fd < 0) return;

    write(fd, "AB", 2);

    int r = ftruncate(fd, 64);
    CHECK("extend: ftruncate to 64", r == 0);
    close(fd);

    struct stat st;
    r = stat(path, &st);
    CHECK("extend: stat returns 0", r == 0);
    CHECK("extend: st_size == 64", st.st_size == 64);

    /* Verify the extended region reads as zeros */
    fd = open(path, O_RDONLY);
    CHECK("extend: reopen", fd >= 0);
    if (fd >= 0) {
        char buf[64];
        memset(buf, 0xFF, sizeof(buf));
        lseek(fd, 2, SEEK_SET);
        ssize_t n = read(fd, buf, 62);
        CHECK("extend: read 62 bytes", n == 62);

        int all_zero = 1;
        int i;
        for (i = 0; i < 62; i++) {
            if (buf[i] != 0) { all_zero = 0; break; }
        }
        CHECK("extend: extended region is zeros", all_zero);
        close(fd);
    }

    unlink(path);
}

/* -----------------------------------------------------------------------
 * 4. bad_fd — ftruncate on invalid fd returns -1
 * ---------------------------------------------------------------------- */

static void test_bad_fd(void)
{
    int r = ftruncate(999, 0);
    CHECK("bad_fd: ftruncate returns -1", r == -1);
}

/* --- Main --- */

int main(void)
{
    printf("=== ftrunctest: ftruncate ===\n");

    test_shrink();
    test_to_zero();
    test_extend();
    test_bad_fd();

    printf("=== ftrunctest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
