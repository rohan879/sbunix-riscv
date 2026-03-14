#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

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

static int contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

int main(void) {
    char buf[1024];
    int n;
    char pidbuf[32];

    n = read_file("/proc/self/status", buf, sizeof(buf));
    check(n > 0, "/proc/self/status readable");
    snprintf(pidbuf, sizeof(pidbuf), "Pid:\t%d", getpid());
    check(contains(buf, "Name:\tprocfstest"), "/proc/self/status has name");
    check(contains(buf, pidbuf), "/proc/self/status has pid");

    n = read_file("/proc/self/cmdline", buf, sizeof(buf));
    check(n > 0, "/proc/self/cmdline readable");
    check(contains(buf, "procfstest"), "/proc/self/cmdline has binary name");

    n = read_file("/proc/self/maps", buf, sizeof(buf));
    check(n > 0, "/proc/self/maps readable");
    check(contains(buf, "r-xp") || contains(buf, "rw-p"), "/proc/self/maps has permissions");

    n = read_file("/proc/version", buf, sizeof(buf));
    check(n > 0, "/proc/version readable");
    check(contains(buf, "SBUnix"), "/proc/version has kernel name");

    n = read_file("/proc/meminfo", buf, sizeof(buf));
    check(n > 0, "/proc/meminfo readable");
    check(contains(buf, "MemTotal:"), "/proc/meminfo has MemTotal");

    n = read_file("/proc/uptime", buf, sizeof(buf));
    check(n > 0, "/proc/uptime readable");
    check(strchr(buf, '.') != NULL, "/proc/uptime has fractional field");

    n = read_file("/proc/cpuinfo", buf, sizeof(buf));
    check(n > 0, "/proc/cpuinfo readable");
    check(contains(buf, "isa"), "/proc/cpuinfo has isa");

    printf("procfstest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
