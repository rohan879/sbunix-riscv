#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static int tests_passed = 0;
static int tests_failed = 0;

static void check(int cond, const char *name) {
    if (cond) { printf("[PASS] fdio: %s\n", name); tests_passed++; }
    else { printf("[FAIL] fdio: %s\n", name); tests_failed++; }
}

int main(void) {
    char buf[64];
    int fd, n;

    /* open/write/read on ext2 */
    fd = open("/mnt/fdio_test", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "open for write");
    n = write(fd, "hello fdio", 10);
    check(n == 10, "write 10 bytes");
    close(fd);

    fd = open("/mnt/fdio_test", O_RDONLY);
    check(fd >= 0, "open for read");
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf));
    check(n == 10, "read 10 bytes");
    check(strcmp(buf, "hello fdio") == 0, "content matches");
    close(fd);

    /* dup */
    fd = open("/mnt/fdio_dup", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "open for dup");
    int fd2 = dup(fd);
    check(fd2 >= 0 && fd2 != fd, "dup returns new fd");
    write(fd2, "dup", 3);
    close(fd2);
    close(fd);

    fd = open("/mnt/fdio_dup", O_RDONLY);
    memset(buf, 0, sizeof(buf));
    read(fd, buf, 64);
    check(strcmp(buf, "dup") == 0, "dup write visible");
    close(fd);

    /* lseek */
    fd = open("/mnt/fdio_test", O_RDONLY);
    check(fd >= 0, "open for lseek");
    off_t pos = lseek(fd, 6, SEEK_SET);
    check(pos == 6, "lseek to 6");
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, 10);
    check(n == 4, "read after lseek");
    check(strcmp(buf, "fdio") == 0, "lseek content");
    close(fd);

    /* pipe */
    int pfd[2];
    int r = pipe(pfd);
    check(r == 0, "pipe");
    write(pfd[1], "pipe", 4);
    memset(buf, 0, sizeof(buf));
    read(pfd[0], buf, 4);
    check(strcmp(buf, "pipe") == 0, "pipe data");
    close(pfd[0]);
    close(pfd[1]);

    /* cleanup */
    unlink("/mnt/fdio_test");
    unlink("/mnt/fdio_dup");

    /* ============================================================
     * Error code tests — kernel must distinguish errors from empty
     * results. A bogus fd must return -1/EBADF, not 0 (empty/EOF).
     * ============================================================ */
    int badfd = 999;

    /* read(bad_fd) must return -1 with EBADF, not 0 (EOF) */
    errno = 0;
    ssize_t rr = read(badfd, buf, 1);
    check(rr == -1, "err: read(bad_fd) returns -1");
    check(errno == EBADF, "err: read(bad_fd) errno==EBADF");

    /* write(bad_fd) must return -1 with EBADF */
    errno = 0;
    ssize_t wr = write(badfd, "x", 1);
    check(wr == -1, "err: write(bad_fd) returns -1");
    check(errno == EBADF, "err: write(bad_fd) errno==EBADF");

    /* close(bad_fd) must return -1 with EBADF */
    errno = 0;
    int cr = close(badfd);
    check(cr == -1, "err: close(bad_fd) returns -1");
    check(errno == EBADF, "err: close(bad_fd) errno==EBADF");

    /* lseek(bad_fd) must return -1 with EBADF */
    errno = 0;
    off_t lr = lseek(badfd, 0, SEEK_SET);
    check(lr == (off_t)-1, "err: lseek(bad_fd) returns -1");
    check(errno == EBADF, "err: lseek(bad_fd) errno==EBADF");

    /* fstat(bad_fd) must return -1 with EBADF */
    errno = 0;
    struct stat st;
    int sr = fstat(badfd, &st);
    check(sr == -1, "err: fstat(bad_fd) returns -1");
    check(errno == EBADF, "err: fstat(bad_fd) errno==EBADF");

    /* lseek on pipe → ESPIPE (not EBADF or 0) */
    pipe(pfd);
    errno = 0;
    lr = lseek(pfd[0], 0, SEEK_SET);
    check(lr == (off_t)-1, "err: lseek(pipe) returns -1");
    check(errno == ESPIPE, "err: lseek(pipe) errno==ESPIPE");
    close(pfd[0]);
    close(pfd[1]);

    /* lseek with invalid whence → EINVAL */
    fd = open("/mnt/fdio_err", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        errno = 0;
        lr = lseek(fd, 0, 99);  /* whence=99 is invalid */
        check(lr == (off_t)-1, "err: lseek(bad_whence) returns -1");
        check(errno == EINVAL, "err: lseek(bad_whence) errno==EINVAL");

        /* lseek to negative offset → EINVAL */
        errno = 0;
        lr = lseek(fd, -100, SEEK_SET);
        check(lr == (off_t)-1, "err: lseek(neg_offset) returns -1");
        check(errno == EINVAL, "err: lseek(neg_offset) errno==EINVAL");

        close(fd);
        unlink("/mnt/fdio_err");
    }

    /* Contrast: read 0 bytes from valid empty file is NOT an error */
    fd = open("/mnt/fdio_empty", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        errno = 0;
        rr = read(fd, buf, 10);
        check(rr == 0, "ok: read(empty_file) returns 0 (EOF)");
        check(errno == 0, "ok: read(empty_file) no errno");
        close(fd);
        unlink("/mnt/fdio_empty");
    }

    printf("fdiotest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
