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

    /* ---- Basic file I/O on ext2 ---- */
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

    /* ---- dup ---- */
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

    /* ---- lseek ---- */
    fd = open("/mnt/fdio_test", O_RDONLY);
    check(fd >= 0, "open for lseek");
    off_t pos = lseek(fd, 6, SEEK_SET);
    check(pos == 6, "lseek to 6");
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, 10);
    check(n == 4, "read after lseek");
    check(strcmp(buf, "fdio") == 0, "lseek content");
    close(fd);

    /* ---- pipe ---- */
    int pfd[2];
    int r = pipe(pfd);
    check(r == 0, "pipe");
    write(pfd[1], "pipe", 4);
    memset(buf, 0, sizeof(buf));
    read(pfd[0], buf, 4);
    check(strcmp(buf, "pipe") == 0, "pipe data");
    close(pfd[0]);
    close(pfd[1]);

    /* ---- dirfd-aware *at syscalls ---- */
    r = mkdir("/mnt/at_base", 0755);
    check(r == 0, "mkdir at_base");

    fd = open("/mnt/at_base", O_RDONLY | O_DIRECTORY);
    check(fd >= 0, "open directory fd");

    if (fd >= 0) {
        int dirfd = fd;
        check(chdir("/") == 0, "chdir away from base dir");

        fd2 = openat(dirfd, "child", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        check(fd2 >= 0, "openat relative to dirfd");
        if (fd2 >= 0) {
            n = write(fd2, "dirfd", 5);
            check(n == 5, "openat write");
            close(fd2);
        }

        fd2 = open("/mnt/at_base/child", O_RDONLY);
        check(fd2 >= 0, "open created child");
        if (fd2 >= 0) {
            memset(buf, 0, sizeof(buf));
            n = read(fd2, buf, sizeof(buf));
            check(n == 5 && strcmp(buf, "dirfd") == 0, "openat created correct file");
            close(fd2);
        }

        {
            struct stat st;
            errno = 0;
            r = fstatat(dirfd, "child", &st, 0);
            check(r == 0, "fstatat relative to dirfd");
        }

        errno = 0;
        r = faccessat(dirfd, "child", F_OK, 0);
        check(r == 0, "faccessat relative to dirfd");

        r = symlink("/mnt/at_base/child", "/mnt/at_base/child_link");
        check(r == 0, "create symlink for readlinkat");
        memset(buf, 0, sizeof(buf));
        n = readlinkat(dirfd, "child_link", buf, sizeof(buf));
        check(n > 0 && strcmp(buf, "/mnt/at_base/child") == 0,
              "readlinkat relative to dirfd");

        errno = 0;
        fd2 = openat(-1, "child", O_RDONLY);
        check(fd2 == -1 && errno == EBADF, "openat bad dirfd returns EBADF");

        close(dirfd);
    }

    /* ---- Invalid fd error returns ---- */

    /* read with bad fd */
    errno = 0;
    n = read(-1, buf, 10);
    check(n == -1 && errno == EBADF, "read(-1) returns EBADF");

    errno = 0;
    n = read(999, buf, 10);
    check(n == -1 && errno == EBADF, "read(999) returns EBADF");

    /* write with bad fd */
    errno = 0;
    n = write(-1, buf, 10);
    check(n == -1 && errno == EBADF, "write(-1) returns EBADF");

    errno = 0;
    n = write(999, buf, 10);
    check(n == -1 && errno == EBADF, "write(999) returns EBADF");

    /* close with bad fd */
    errno = 0;
    r = close(-1);
    check(r == -1 && errno == EBADF, "close(-1) returns EBADF");

    errno = 0;
    r = close(999);
    check(r == -1 && errno == EBADF, "close(999) returns EBADF");

    /* double close */
    fd = open("/mnt/fdio_test", O_RDONLY);
    close(fd);
    errno = 0;
    r = close(fd);
    check(r == -1 && errno == EBADF, "double close returns EBADF");

    /* lseek with bad fd */
    errno = 0;
    pos = lseek(-1, 0, SEEK_SET);
    check(pos == -1 && errno == EBADF, "lseek(-1) returns EBADF");

    /* lseek on pipe (ESPIPE) */
    pipe(pfd);
    errno = 0;
    pos = lseek(pfd[0], 0, SEEK_SET);
    check(pos == -1 && errno == ESPIPE, "lseek on pipe returns ESPIPE");
    close(pfd[0]);
    close(pfd[1]);

    /* dup with bad fd */
    errno = 0;
    fd2 = dup(-1);
    check(fd2 == -1 && errno == EBADF, "dup(-1) returns EBADF");

    /* fstat with bad fd */
    {
        struct stat st;
        errno = 0;
        r = fstat(-1, &st);
        check(r == -1 && errno == EBADF, "fstat(-1) returns EBADF");
    }

    /* fsync with bad fd */
    errno = 0;
    r = fsync(-1);
    check(r == -1 && errno == EBADF, "fsync(-1) returns EBADF");

    /* cleanup */
    unlink("/mnt/at_base/child_link");
    unlink("/mnt/at_base/child");
    rmdir("/mnt/at_base");
    unlink("/mnt/fdio_test");
    unlink("/mnt/fdio_dup");

    printf("fdiotest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
