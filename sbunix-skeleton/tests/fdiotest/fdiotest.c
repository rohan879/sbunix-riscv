/*
 * fdiotest.c -- File descriptor I/O and error handling tests
 *
 *
 * Key design principle: "kernel should distinguish between 'nothing
 * happened' and 'request was invalid'. Invalid resource handles should
 * return errors, not succeed."
 *
 * Run from the shell:  fdiotest
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * 1. read_bad_fd — read(999) must return -1 with EBADF
 * ---------------------------------------------------------------------- */
static void test_read_bad_fd(void)
{
    char buf[16];
    errno = 0;
    ssize_t r = read(999, buf, sizeof(buf));
    CHECK("read_bad_fd: returns -1", r == -1);
    CHECK("read_bad_fd: errno==EBADF", errno == EBADF);
}

/* -----------------------------------------------------------------------
 * 2. write_bad_fd — write(999) must return -1 with EBADF
 * ---------------------------------------------------------------------- */
static void test_write_bad_fd(void)
{
    errno = 0;
    ssize_t r = write(999, "x", 1);
    CHECK("write_bad_fd: returns -1", r == -1);
    CHECK("write_bad_fd: errno==EBADF", errno == EBADF);
}

/* -----------------------------------------------------------------------
 * 3. close_bad_fd — close(999) must return -1 with EBADF
 * ---------------------------------------------------------------------- */
static void test_close_bad_fd(void)
{
    errno = 0;
    int r = close(999);
    CHECK("close_bad_fd: returns -1", r == -1);
    CHECK("close_bad_fd: errno==EBADF", errno == EBADF);
}

/* -----------------------------------------------------------------------
 * 4. fstat_bad_fd — fstat(999) must return -1 with EBADF
 * ---------------------------------------------------------------------- */
static void test_fstat_bad_fd(void)
{
    struct stat st;
    errno = 0;
    int r = fstat(999, &st);
    CHECK("fstat_bad_fd: returns -1", r == -1);
    CHECK("fstat_bad_fd: errno==EBADF", errno == EBADF);
}

/* -----------------------------------------------------------------------
 * 5. lseek_bad_fd — lseek(999) must return -1 with EBADF
 * ---------------------------------------------------------------------- */
static void test_lseek_bad_fd(void)
{
    errno = 0;
    off_t r = lseek(999, 0, SEEK_SET);
    CHECK("lseek_bad_fd: returns -1", r == (off_t)-1);
    CHECK("lseek_bad_fd: errno==EBADF", errno == EBADF);
}

/* -----------------------------------------------------------------------
 * 6. dup_bad_fd — dup(999) must return -1 with EBADF
 * ---------------------------------------------------------------------- */
static void test_dup_bad_fd(void)
{
    errno = 0;
    int r = dup(999);
    CHECK("dup_bad_fd: returns -1", r == -1);
    CHECK("dup_bad_fd: errno==EBADF", errno == EBADF);
}

/* -----------------------------------------------------------------------
 * 7. read_closed_fd — open, close, then read returns EBADF
 * ---------------------------------------------------------------------- */
static void test_read_closed_fd(void)
{
    const char *path = "/mnt/fdio_closed";
    unlink(path);
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    CHECK("read_closed_fd: open", fd >= 0);
    if (fd < 0) return;

    write(fd, "data", 4);
    close(fd);

    /* Reopen to get a known fd, then close it */
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        unlink(path);
        CHECK("read_closed_fd: reopen", 0);
        return;
    }
    int saved_fd = fd;
    close(fd);

    /* Now read from the closed fd */
    char buf[16];
    errno = 0;
    ssize_t r = read(saved_fd, buf, sizeof(buf));
    CHECK("read_closed_fd: returns -1", r == -1);
    CHECK("read_closed_fd: errno==EBADF", errno == EBADF);

    unlink(path);
}

/* -----------------------------------------------------------------------
 * 8. write_to_readonly — open O_RDONLY, write returns EBADF
 * ---------------------------------------------------------------------- */
static void test_write_to_readonly(void)
{
    const char *path = "/mnt/fdio_rdonly";
    unlink(path);

    /* Create the file first */
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) {
        write(fd, "data", 4);
        close(fd);
    }

    fd = open(path, O_RDONLY);
    CHECK("write_to_readonly: open O_RDONLY", fd >= 0);
    if (fd < 0) {
        unlink(path);
        return;
    }

    errno = 0;
    ssize_t r = write(fd, "x", 1);
    CHECK("write_to_readonly: returns -1", r == -1);
    CHECK("write_to_readonly: errno==EBADF", errno == EBADF);
    close(fd);
    unlink(path);
}

/* -----------------------------------------------------------------------
 * 9. read_from_writeonly — open O_WRONLY, read returns EBADF
 * ---------------------------------------------------------------------- */
static void test_read_from_writeonly(void)
{
    const char *path = "/mnt/fdio_wronly";
    unlink(path);

    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    CHECK("read_from_writeonly: open O_WRONLY", fd >= 0);
    if (fd < 0) return;

    char buf[16];
    errno = 0;
    ssize_t r = read(fd, buf, sizeof(buf));
    CHECK("read_from_writeonly: returns -1", r == -1);
    CHECK("read_from_writeonly: errno==EBADF", errno == EBADF);
    close(fd);
    unlink(path);
}

/* -----------------------------------------------------------------------
 * 10. lseek_pipe — lseek on a pipe fd returns ESPIPE
 * ---------------------------------------------------------------------- */
static void test_lseek_pipe(void)
{
    int pfd[2];
    int r = pipe(pfd);
    CHECK("lseek_pipe: pipe()", r == 0);
    if (r != 0) return;

    errno = 0;
    off_t pos = lseek(pfd[0], 0, SEEK_SET);
    CHECK("lseek_pipe: returns -1", pos == (off_t)-1);
    CHECK("lseek_pipe: errno==ESPIPE", errno == ESPIPE);

    /* Also test write end */
    errno = 0;
    pos = lseek(pfd[1], 0, SEEK_SET);
    CHECK("lseek_pipe: write end returns -1", pos == (off_t)-1);
    CHECK("lseek_pipe: write end errno==ESPIPE", errno == ESPIPE);

    close(pfd[0]);
    close(pfd[1]);
}

/* -----------------------------------------------------------------------
 * 11. open_nonexistent — open missing file returns ENOENT
 * ---------------------------------------------------------------------- */
static void test_open_nonexistent(void)
{
    errno = 0;
    int fd = open("/no/such/file", O_RDONLY);
    CHECK("open_nonexistent: returns -1", fd == -1);
    CHECK("open_nonexistent: errno==ENOENT", errno == ENOENT);
    if (fd >= 0) close(fd);
}

/* -----------------------------------------------------------------------
 * 12. open_dir_rdwr — open("/", O_RDWR) returns EISDIR
 * ---------------------------------------------------------------------- */
static void test_open_dir_rdwr(void)
{
    errno = 0;
    int fd = open("/", O_RDWR);
    CHECK("open_dir_rdwr: returns -1", fd == -1);
    CHECK("open_dir_rdwr: errno==EISDIR", errno == EISDIR);
    if (fd >= 0) close(fd);
}

/* -----------------------------------------------------------------------
 * 13. dup_valid — dup(stdout) returns new fd, both work
 * ---------------------------------------------------------------------- */
static void test_dup_valid(void)
{
    /* Use a file instead of stdout to avoid interleaved output */
    const char *path = "/mnt/fdio_dup_valid";
    unlink(path);

    int fd = open(path, O_CREAT | O_RDWR, 0644);
    CHECK("dup_valid: open", fd >= 0);
    if (fd < 0) return;

    int fd2 = dup(fd);
    CHECK("dup_valid: dup returns >= 0", fd2 >= 0);
    CHECK("dup_valid: dup returns different fd", fd2 != fd);

    /* Write via both fds */
    write(fd, "AAA", 3);
    write(fd2, "BBB", 3);
    close(fd2);
    close(fd);

    /* Read back and verify combined content */
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        char buf[16];
        memset(buf, 0, sizeof(buf));
        ssize_t n = read(fd, buf, sizeof(buf));
        CHECK("dup_valid: writes visible (6 bytes)", n == 6);
        CHECK("dup_valid: content is AAABBB",
              memcmp(buf, "AAABBB", 6) == 0);
        close(fd);
    }
    unlink(path);
}

/* -----------------------------------------------------------------------
 * 14. pipe_read_write — basic pipe round-trip
 * ---------------------------------------------------------------------- */
static void test_pipe_read_write(void)
{
    int pfd[2];
    int r = pipe(pfd);
    CHECK("pipe_read_write: pipe()", r == 0);
    if (r != 0) return;

    const char *msg = "test_pipe_data";
    ssize_t nw = write(pfd[1], msg, strlen(msg));
    CHECK("pipe_read_write: write succeeds", nw == (ssize_t)strlen(msg));

    char buf[64];
    memset(buf, 0, sizeof(buf));
    ssize_t nr = read(pfd[0], buf, sizeof(buf));
    CHECK("pipe_read_write: read matches write",
          nr == (ssize_t)strlen(msg) && memcmp(buf, msg, strlen(msg)) == 0);

    close(pfd[0]);
    close(pfd[1]);
}

/* -----------------------------------------------------------------------
 * 15. pipe_close_write_sigpipe — write to pipe with closed read end
 *     gets SIGPIPE or returns EPIPE.
 * ---------------------------------------------------------------------- */
static void test_pipe_close_write_sigpipe(void)
{
    /*
     * Run in a child so SIGPIPE does not kill the test harness.
     * The child:
     *   1. Creates a pipe
     *   2. Closes the read end
     *   3. Ignores SIGPIPE so write() returns EPIPE instead of dying
     *   4. Writes to the pipe write end
     *   5. Exits 0 if write returned -1/EPIPE, 1 otherwise
     *
     * Alternatively, if the child does NOT ignore SIGPIPE, the default
     * action kills it with SIGPIPE, which is also acceptable.
     */
    pid_t pid = fork();
    if (pid == 0) {
        /* Approach: ignore SIGPIPE and check EPIPE */
        struct sigaction sa;
        sa.sa_handler = SIG_IGN;
        sa.sa_mask = 0;
        sa.sa_flags = 0;
        sa.sa_restorer = 0;
        sigaction(SIGPIPE, &sa, 0);

        int pfd[2];
        pipe(pfd);
        close(pfd[0]); /* close read end */

        errno = 0;
        ssize_t r = write(pfd[1], "x", 1);
        close(pfd[1]);

        if (r == -1 && errno == EPIPE)
            _exit(0);
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        CHECK("pipe_close_write_sigpipe: write returns EPIPE",
              WEXITSTATUS(status) == 0);
    } else if (WIFSIGNALED(status)) {
        /* Child was killed by SIGPIPE — also acceptable */
        CHECK("pipe_close_write_sigpipe: child killed by SIGPIPE",
              WTERMSIG(status) == SIGPIPE);
    } else {
        CHECK("pipe_close_write_sigpipe: unexpected status", 0);
    }
}

/* -----------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int main(void)
{
    printf("=== fdiotest: file descriptor I/O and error handling ===\n");

    /* Bad fd error tests (critical for POSIX compliance) */
    test_read_bad_fd();
    test_write_bad_fd();
    test_close_bad_fd();
    test_fstat_bad_fd();
    test_lseek_bad_fd();
    test_dup_bad_fd();

    /* Use-after-close and access mode violations */
    test_read_closed_fd();
    test_write_to_readonly();
    test_read_from_writeonly();

    /* Pipe-specific errors */
    test_lseek_pipe();

    /* Filesystem errors */
    test_open_nonexistent();
    test_open_dir_rdwr();

    /* Positive / functional tests */
    test_dup_valid();
    test_pipe_read_write();
    test_pipe_close_write_sigpipe();

    printf("=== fdiotest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
