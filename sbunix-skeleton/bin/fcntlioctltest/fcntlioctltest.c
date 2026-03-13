#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { \
        printf("  PASS  %s\n", label); \
        g_pass++; \
    } else { \
        printf("  FAIL  %s\n", label); \
        g_fail++; \
    } \
} while (0)

static void test_fcntl(void) {
    int fd = open("/bin/echo", O_RDONLY);
    CHECK("open /bin/echo", fd >= 0);
    if (fd < 0)
        return;

    int flags = fcntl(fd, F_GETFL);
    CHECK("fcntl F_GETFL", flags >= 0);
    CHECK("fcntl access mode", (flags & O_ACCMODE) == O_RDONLY);
    CHECK("fcntl F_GETFD default", fcntl(fd, F_GETFD) == 0);
    CHECK("fcntl F_SETFD cloexec", fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
    CHECK("fcntl F_GETFD cloexec", fcntl(fd, F_GETFD) == FD_CLOEXEC);
    CHECK("fcntl F_SETFL nonblock", fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    flags = fcntl(fd, F_GETFL);
    CHECK("fcntl kept access mode", (flags & O_ACCMODE) == O_RDONLY);
    CHECK("fcntl set nonblock", (flags & O_NONBLOCK) != 0);
    errno = 0;
    CHECK("fcntl invalid cmd", fcntl(fd, 9999, 0) == -1 && errno == EINVAL);
    close(fd);

    fd = open("/bin/echo", O_RDONLY);
    CHECK("open /bin/echo for dupfd", fd >= 0);
    if (fd >= 0) {
        int dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, 7);
        CHECK("fcntl F_DUPFD_CLOEXEC", dup_fd >= 7);
        if (dup_fd >= 0) {
            CHECK("dupfd cloexec set", fcntl(dup_fd, F_GETFD) == FD_CLOEXEC);
            close(dup_fd);
        }
        close(fd);
    }
}

static void test_ioctl(void) {
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    CHECK("ioctl TIOCGWINSZ tty", ioctl(0, TIOCGWINSZ, &ws) == 0);
    CHECK("ioctl winsize nonzero", ws.ws_row > 0 && ws.ws_col > 0);

    struct winsize set_ws = { .ws_row = 30, .ws_col = 100, .ws_xpixel = 0, .ws_ypixel = 0 };
    CHECK("ioctl TIOCSWINSZ tty", ioctl(0, TIOCSWINSZ, &set_ws) == 0);
    memset(&ws, 0, sizeof(ws));
    CHECK("ioctl winsize roundtrip", ioctl(0, TIOCGWINSZ, &ws) == 0 &&
                                     ws.ws_row == 30 && ws.ws_col == 100);

    int available = -1;
    CHECK("ioctl FIONREAD tty", ioctl(0, FIONREAD, &available) == 0);
    CHECK("ioctl FIONREAD tty nonnegative", available >= 0);

    int pfd[2] = { -1, -1 };
    int pipe_rc = pipe(pfd);
    CHECK("pipe", pipe_rc == 0);
    if (pipe_rc == 0) {
        CHECK("pipe write", write(pfd[1], "xyz", 3) == 3);
        int pipe_available = -1;
        CHECK("ioctl FIONREAD pipe", ioctl(pfd[0], FIONREAD, &pipe_available) == 0);
        CHECK("ioctl FIONREAD pipe count", pipe_available == 3);
        close(pfd[0]);
        close(pfd[1]);
    }

    int fd = open("/bin/echo", O_RDONLY);
    CHECK("open /bin/echo for ENOTTY", fd >= 0);
    if (fd >= 0) {
        errno = 0;
        CHECK("ioctl regular file ENOTTY", ioctl(fd, TIOCGWINSZ, &ws) == -1 && errno == ENOTTY);
        close(fd);
    }
}

int main(void) {
    printf("fcntlioctltest: start\n");
    test_fcntl();
    test_ioctl();
    printf("fcntlioctltest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
