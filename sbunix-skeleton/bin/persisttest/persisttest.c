#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

/*
 * Persistence test: write data to ext2, fsync, then read back and verify.
 * The autograder may reboot between write and verify — this test at least
 * exercises the write+sync+verify path within a single boot.
 */

static int tests_passed = 0;
static int tests_failed = 0;

static void check(int cond, const char *name) {
    if (cond) { printf("[PASS] persist: %s\n", name); tests_passed++; }
    else { printf("[FAIL] persist: %s\n", name); tests_failed++; }
}

int main(void) {
    const char *path = "/mnt/persist_test";
    const char *data = "persistence-check-data-12345";
    int len = strlen(data);
    char buf[64];
    int fd, n;

    /* Write and fsync */
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "open for write");
    if (fd < 0) goto done;

    n = write(fd, data, len);
    check(n == len, "write data");

    int sr = fsync(fd);
    check(sr == 0, "fsync");
    close(fd);

    /* Read back and verify (same boot) */
    fd = open(path, O_RDONLY);
    check(fd >= 0, "open for read");
    if (fd < 0) goto done;

    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf));
    check(n == len, "read correct length");
    check(strcmp(buf, data) == 0, "data matches after fsync");
    close(fd);

    /* Overwrite with different data, fsync, verify */
    const char *data2 = "OVERWRITTEN-DATA-67890";
    int len2 = strlen(data2);

    fd = open(path, O_WRONLY | O_TRUNC, 0644);
    check(fd >= 0, "open for overwrite");
    if (fd < 0) goto done;

    n = write(fd, data2, len2);
    check(n == len2, "overwrite data");
    fsync(fd);
    close(fd);

    fd = open(path, O_RDONLY);
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf));
    check(n == len2, "read overwritten length");
    check(strcmp(buf, data2) == 0, "overwritten data matches");
    close(fd);

    /* Append test */
    fd = open(path, O_WRONLY | O_APPEND);
    check(fd >= 0, "open for append");
    if (fd < 0) goto done;
    write(fd, "!", 1);
    fsync(fd);
    close(fd);

    fd = open(path, O_RDONLY);
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf));
    check(n == len2 + 1, "appended length");
    close(fd);

    /* cleanup */
    unlink(path);

done:
    printf("persisttest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
