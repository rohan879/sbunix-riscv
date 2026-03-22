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

/* Simple atoi: parse leading decimal integer from string */
static int simple_atoi(const char *s) {
    int val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return val;
}

int main(void) {
    char buf[1024];
    int n;
    pid_t mypid = getpid();

    /* --- Existing tests --- */

    n = read_file("/proc/self/status", buf, sizeof(buf));
    check(n > 0, "/proc/self/status readable");

    /* Verify /proc/self/status contains process name */
    if (n > 0) {
        check(strstr(buf, "Name:") != NULL, "/proc/self/status contains Name field");
        check(strstr(buf, "procfstest") != NULL, "/proc/self/status contains process name");
    }

    n = read_file("/proc/self/cmdline", buf, sizeof(buf));
    check(n > 0, "/proc/self/cmdline readable");

    n = read_file("/proc/self/maps", buf, sizeof(buf));
    check(n > 0, "/proc/self/maps readable");

    n = read_file("/proc/version", buf, sizeof(buf));
    check(n > 0, "/proc/version readable");

    /* --- /proc/meminfo: verify MemTotal line exists --- */
    n = read_file("/proc/meminfo", buf, sizeof(buf));
    check(n > 0, "/proc/meminfo readable");
    if (n > 0) {
        check(strstr(buf, "MemTotal:") != NULL, "/proc/meminfo contains MemTotal");
    }

    /* --- /proc/uptime: verify it returns a number --- */
    n = read_file("/proc/uptime", buf, sizeof(buf));
    check(n > 0, "/proc/uptime readable");
    if (n > 0) {
        int uptime_val = simple_atoi(buf);
        check(uptime_val >= 0, "/proc/uptime returns a number");
    }

    n = read_file("/proc/cpuinfo", buf, sizeof(buf));
    check(n > 0, "/proc/cpuinfo readable");

    /* --- /proc/self/stat: single-line format, PID is first field --- */
    n = read_file("/proc/self/stat", buf, sizeof(buf));
    check(n > 0, "/proc/self/stat readable");
    if (n > 0) {
        int stat_pid = simple_atoi(buf);
        check(stat_pid == mypid, "/proc/self/stat first field is current PID");
        /* Verify it contains the process name in parentheses */
        check(strstr(buf, "(procfstest)") != NULL, "/proc/self/stat contains (procfstest)");
    }

    /* --- readlink /proc/self returns PID string --- */
    n = readlink("/proc/self", buf, sizeof(buf) - 1);
    check(n > 0, "readlink /proc/self returns data");
    if (n > 0) {
        buf[n] = '\0';
        int link_pid = simple_atoi(buf);
        check(link_pid == mypid, "readlink /proc/self returns current PID");
    }

    /* --- readlink /proc/self/exe returns executable path --- */
    n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    check(n > 0, "readlink /proc/self/exe returns data");
    if (n > 0) {
        buf[n] = '\0';
        /* The path should contain "procfstest" */
        check(strstr(buf, "procfstest") != NULL, "readlink /proc/self/exe contains procfstest");
    }

    /* --- readlink /proc/self/cwd returns "/" --- */
    n = readlink("/proc/self/cwd", buf, sizeof(buf) - 1);
    check(n > 0, "readlink /proc/self/cwd returns data");
    if (n > 0) {
        buf[n] = '\0';
        check(strcmp(buf, "/") == 0, "readlink /proc/self/cwd returns /");
    }

    printf("procfstest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
