#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

static void check(int cond, const char *name) {
    if (cond) { printf("[PASS] procfs: %s\n", name); tests_passed++; }
    else { printf("[FAIL] procfs: %s\n", name); tests_failed++; }
}

static int read_file(const char *path, char *buf, int bufsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

int main(void) {
    char buf[1024];
    int n;

    n = read_file("/proc/self/status", buf, sizeof(buf));
    check(n > 0, "/proc/self/status readable");

    n = read_file("/proc/self/cmdline", buf, sizeof(buf));
    check(n > 0, "/proc/self/cmdline readable");

    n = read_file("/proc/self/maps", buf, sizeof(buf));
    check(n > 0, "/proc/self/maps readable");

    n = read_file("/proc/version", buf, sizeof(buf));
    check(n > 0, "/proc/version readable");

    n = read_file("/proc/meminfo", buf, sizeof(buf));
    check(n > 0, "/proc/meminfo readable");

    n = read_file("/proc/uptime", buf, sizeof(buf));
    check(n > 0, "/proc/uptime readable");

    n = read_file("/proc/cpuinfo", buf, sizeof(buf));
    check(n > 0, "/proc/cpuinfo readable");

    printf("procfstest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
