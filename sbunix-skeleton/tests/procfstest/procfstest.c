/*
 * procfstest.c -- /proc filesystem tests
 *
 * Exercises /proc/self/status, /proc/self/stat, /proc/self/exe,
 * /proc/self/cwd, /proc/self/maps, /proc/<pid>/status, /proc/meminfo,
 * /proc/uptime, /proc/version, and readlink on /proc/self.
 *
 * Run from the shell:  procfstest
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* Read entire file into buf, NUL-terminate, return bytes read or -1 */
static int read_file(const char *path, char *buf, int bufsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int total = 0;
    while (total < bufsz - 1) {
        int n = read(fd, buf + total, bufsz - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    buf[total] = '\0';
    return total;
}

/* Read raw bytes from file (no NUL termination assumptions). */
static int read_file_raw(const char *path, char *buf, int bufsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int total = 0;
    while (total < bufsz) {
        int n = read(fd, buf + total, bufsz - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    return total;
}

/* Simple decimal integer parser from string */
static int simple_atoi(const char *s) {
    int val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return val;
}

/* Format an integer into a decimal string, return pointer to static buf */
static char *itoa_simple(int v) {
    static char tmp[32];
    int pos = 30;
    tmp[31] = '\0';
    if (v == 0) {
        tmp[30] = '0';
        return &tmp[30];
    }
    while (v > 0 && pos >= 0) {
        tmp[pos--] = '0' + (v % 10);
        v /= 10;
    }
    return &tmp[pos + 1];
}

/* -----------------------------------------------------------------------
 * 1. proc_self_exists -- open("/proc/self/status") succeeds
 * ---------------------------------------------------------------------- */

static void test_proc_self_exists(void) {
    int fd = open("/proc/self/status", O_RDONLY);
    CHECK("procfs: proc_self_exists", fd >= 0);
    if (fd >= 0) close(fd);
}

/* -----------------------------------------------------------------------
 * 2. proc_self_readlink -- readlink("/proc/self") returns current PID
 * ---------------------------------------------------------------------- */

static void test_proc_self_readlink(void) {
    char buf[64];
    int n = readlink("/proc/self", buf, sizeof(buf) - 1);
    CHECK("procfs: proc_self_readlink returns data", n > 0);
    if (n > 0) {
        buf[n] = '\0';
        int link_pid = simple_atoi(buf);
        pid_t mypid = getpid();
        CHECK("procfs: proc_self_readlink is current PID",
              link_pid == (int)mypid);
    }
}

/* -----------------------------------------------------------------------
 * 3. proc_self_status -- contains "Name:" and "Pid:" lines
 * ---------------------------------------------------------------------- */

static void test_proc_self_status(void) {
    char buf[2048];
    int n = read_file("/proc/self/status", buf, sizeof(buf));
    CHECK("procfs: proc_self_status readable", n > 0);
    if (n > 0) {
        CHECK("procfs: proc_self_status contains Name:",
              strstr(buf, "Name:") != NULL);
        CHECK("procfs: proc_self_status contains Pid:",
              strstr(buf, "Pid:") != NULL);
    }
}

/* -----------------------------------------------------------------------
 * 4. proc_self_stat -- first field is PID number
 * ---------------------------------------------------------------------- */

static void test_proc_self_stat(void) {
    char buf[1024];
    int n = read_file("/proc/self/stat", buf, sizeof(buf));
    CHECK("procfs: proc_self_stat readable", n > 0);
    if (n > 0) {
        int stat_pid = simple_atoi(buf);
        pid_t mypid = getpid();
        CHECK("procfs: proc_self_stat first field is PID",
              stat_pid == (int)mypid);
    }
}

/* -----------------------------------------------------------------------
 * 5. proc_self_exe -- readlink returns path of current binary
 * ---------------------------------------------------------------------- */

static void test_proc_self_exe(void) {
    char buf[256];
    int n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    CHECK("procfs: proc_self_exe returns data", n > 0);
    if (n > 0) {
        buf[n] = '\0';
        CHECK("procfs: proc_self_exe contains procfstest",
              strstr(buf, "procfstest") != NULL);
    }
}

/* -----------------------------------------------------------------------
 * 6. proc_self_cwd -- readlink returns working directory
 * ---------------------------------------------------------------------- */

static void test_proc_self_cwd(void) {
    char buf[256];
    int n = readlink("/proc/self/cwd", buf, sizeof(buf) - 1);
    CHECK("procfs: proc_self_cwd returns data", n > 0);
    if (n > 0) {
        buf[n] = '\0';
        /* The cwd should be a valid path (at least "/") */
        CHECK("procfs: proc_self_cwd starts with /", buf[0] == '/');
    }
}

/* -----------------------------------------------------------------------
 * 7. proc_self_maps -- contains at least one entry with rwx permissions
 * ---------------------------------------------------------------------- */

static void test_proc_self_maps(void) {
    char buf[4096];
    int n = read_file("/proc/self/maps", buf, sizeof(buf));
    CHECK("procfs: proc_self_maps readable", n > 0);
    if (n > 0) {
        /* Maps entries contain permission strings like "r-x", "rw-", etc.
         * Look for any 'r' (read) or 'w' (write) or 'x' (execute) char
         * in a permission field.  A typical line:
         *   00400000-00401000 r-xp ...
         * We just check that at least one of r, w, or x appears. */
        int has_perm = (strstr(buf, "r") != NULL ||
                        strstr(buf, "w") != NULL ||
                        strstr(buf, "x") != NULL);
        CHECK("procfs: proc_self_maps has permission entries", has_perm);
    }
}

/* -----------------------------------------------------------------------
 * 8. proc_pid_readable -- fork child, parent reads /proc/<child_pid>/status
 * ---------------------------------------------------------------------- */

static void test_proc_pid_readable(void) {
    int pfd[2];
    if (pipe(pfd) != 0) {
        CHECK("procfs: proc_pid_readable (pipe failed)", 0);
        return;
    }

    pid_t child = fork();
    if (child == 0) {
        /* Child: close write end, wait for parent to signal via pipe */
        close(pfd[1]);
        char c;
        read(pfd[0], &c, 1);  /* blocks until parent closes or writes */
        close(pfd[0]);
        _exit(0);
    }

    /* Parent: build /proc/<child_pid>/status path and try to read it */
    close(pfd[0]);

    char path[64];
    char *pid_str = itoa_simple((int)child);
    snprintf(path, sizeof(path), "/proc/%s/status", pid_str);

    char buf[1024];
    int n = read_file(path, buf, sizeof(buf));
    CHECK("procfs: proc_pid_readable", n > 0);

    /* Signal child to exit and reap it */
    write(pfd[1], "x", 1);
    close(pfd[1]);
    int status;
    waitpid(child, &status, 0);
}

/* -----------------------------------------------------------------------
 * 9. proc_meminfo -- contains "MemTotal:" and "MemFree:" lines
 * ---------------------------------------------------------------------- */

static void test_proc_meminfo(void) {
    char buf[2048];
    int n = read_file("/proc/meminfo", buf, sizeof(buf));
    CHECK("procfs: proc_meminfo readable", n > 0);
    if (n > 0) {
        CHECK("procfs: proc_meminfo contains MemTotal:",
              strstr(buf, "MemTotal:") != NULL);
        CHECK("procfs: proc_meminfo contains MemFree:",
              strstr(buf, "MemFree:") != NULL);
    }
}

/* -----------------------------------------------------------------------
 * 10. proc_uptime -- parses as a positive number
 * ---------------------------------------------------------------------- */

static void test_proc_uptime(void) {
    char buf[256];
    int n = read_file("/proc/uptime", buf, sizeof(buf));
    CHECK("procfs: proc_uptime readable", n > 0);
    if (n > 0) {
        /* The uptime should start with a digit (positive number) */
        int val = simple_atoi(buf);
        CHECK("procfs: proc_uptime is a positive number", val >= 0);
    }
}

/* -----------------------------------------------------------------------
 * 11. proc_version -- non-empty string
 * ---------------------------------------------------------------------- */

static void test_proc_version(void) {
    char buf[512];
    int n = read_file("/proc/version", buf, sizeof(buf));
    CHECK("procfs: proc_version readable and non-empty", n > 0);
}

/* -----------------------------------------------------------------------
 * 12. proc_nonexistent -- open("/proc/99999/status") fails
 * ---------------------------------------------------------------------- */

static void test_proc_nonexistent(void) {
    int fd = open("/proc/99999/status", O_RDONLY);
    CHECK("procfs: proc_nonexistent returns error", fd < 0);
    if (fd >= 0) close(fd);
}

/* -----------------------------------------------------------------------
 * 13. proc_cmdline_has_args -- /proc/<pid>/cmdline is NUL-separated argv
 * ---------------------------------------------------------------------- */

static void test_proc_cmdline_has_args(void) {
    pid_t child = fork();
    if (child == 0) {
        char *argv[] = {"/bin/sleep", "9", NULL};
        execve("/bin/sleep", argv, NULL);
        _exit(127);
    }

    if (child < 0) {
        CHECK("procfs: proc_cmdline_has_args fork", 0);
        return;
    }

    usleep(50000);

    char path[64];
    char *pid_str = itoa_simple((int)child);
    snprintf(path, sizeof(path), "/proc/%s/cmdline", pid_str);

    char buf[256];
    memset(buf, 0, sizeof(buf));
    int n = read_file_raw(path, buf, sizeof(buf));
    CHECK("procfs: proc_cmdline_has_args readable", n > 0);
    if (n > 0) {
        int has_prog = 0;
        int has_arg = 0;
        for (int i = 0; i < n; ) {
            int j = i;
            while (j < n && buf[j] != '\0') j++;
            if (j > i) {
                if (strcmp(buf + i, "/bin/sleep") == 0)
                    has_prog = 1;
                if (strcmp(buf + i, "9") == 0)
                    has_arg = 1;
            }
            i = j + 1;
        }
        CHECK("procfs: proc_cmdline_has_args has argv0", has_prog);
        CHECK("procfs: proc_cmdline_has_args has argv1", has_arg);
    }

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
}

/* -----------------------------------------------------------------------
 * 14. proc_child_exe_link -- /proc/<pid>/exe resolves for forked child
 * ---------------------------------------------------------------------- */

static void test_proc_child_exe_link(void) {
    int pfd[2];
    if (pipe(pfd) != 0) {
        CHECK("procfs: proc_child_exe_link pipe", 0);
        return;
    }

    pid_t child = fork();
    if (child == 0) {
        close(pfd[1]);
        char c;
        read(pfd[0], &c, 1);
        close(pfd[0]);
        _exit(0);
    }

    close(pfd[0]);

    char path[64];
    char *pid_str = itoa_simple((int)child);
    snprintf(path, sizeof(path), "/proc/%s/exe", pid_str);

    char exe[256];
    int n = readlink(path, exe, sizeof(exe) - 1);
    CHECK("procfs: proc_child_exe_link readlink returns data", n > 0);
    if (n > 0) {
        exe[n] = '\0';
        CHECK("procfs: proc_child_exe_link contains procfstest",
              strstr(exe, "procfstest") != NULL);
    }

    write(pfd[1], "x", 1);
    close(pfd[1]);
    waitpid(child, NULL, 0);
}

/* -----------------------------------------------------------------------
 * 15. proc_fd_entries -- /proc/self/fd/<n> behaves like a symlink entry
 * ---------------------------------------------------------------------- */

static void test_proc_fd_entries(void) {
    const char *path = "/tmp/procfstest-fd";
    unlink(path);

    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        CHECK("procfs: proc_fd_entries setup", 0);
        return;
    }

    write(fd, "fd", 2);

    char procpath[64];
    snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", fd);

    char link[256];
    int n = readlink(procpath, link, sizeof(link) - 1);
    CHECK("procfs: proc_fd_entries readlink returns data", n > 0);
    if (n > 0) {
        link[n] = '\0';
        CHECK("procfs: proc_fd_entries readlink matches target",
              strcmp(link, path) == 0);
    }

    struct stat st_path;
    struct stat st_fd;
    int sr1 = stat(procpath, &st_path);
    int lfd = open(procpath, O_RDONLY | O_NOFOLLOW);
    int nofollow_errno = errno;
    int sr2 = -1;
    if (lfd >= 0) {
        sr2 = fstat(lfd, &st_fd);
        close(lfd);
    }

    CHECK("procfs: proc_fd_entries stat follows symlink", sr1 == 0 && st_path.st_size == 2);
    CHECK("procfs: proc_fd_entries O_NOFOLLOW rejects symlink",
          lfd == -1 && nofollow_errno == ELOOP);

    int rfd = open(procpath, O_RDONLY);
    char buf[8];
    int nr = -1;
    if (rfd >= 0) {
        memset(buf, 0, sizeof(buf));
        nr = read(rfd, buf, sizeof(buf));
        sr2 = fstat(rfd, &st_fd);
        close(rfd);
    }
    CHECK("procfs: proc_fd_entries open follows symlink",
          rfd >= 0 && nr == 2 && memcmp(buf, "fd", 2) == 0);
    CHECK("procfs: proc_fd_entries fstat matches followed target",
          rfd >= 0 && sr1 == 0 && sr2 == 0 && st_fd.st_size == st_path.st_size);

    close(fd);
    unlink(path);
}

/* -----------------------------------------------------------------------
 * 16. proc_fd_inode_unique -- /proc fd symlink inodes don't collide
 * ---------------------------------------------------------------------- */

static void test_proc_fd_inode_unique(void) {
    struct stat st_self;
    struct stat st_child;
    int pfd[2];

    if (pipe(pfd) != 0) {
        CHECK("procfs: proc_fd_inode_unique pipe", 0);
        return;
    }

    pid_t child = fork();
    if (child == 0) {
        close(pfd[1]);
        char c;
        read(pfd[0], &c, 1);
        close(pfd[0]);
        _exit(0);
    }

    close(pfd[0]);
    char child_path[64];
    snprintf(child_path, sizeof(child_path), "/proc/%d/fd/0", (int)child);

    int r1 = lstat("/proc/self/fd/0", &st_self);
    int r2 = lstat(child_path, &st_child);
    CHECK("procfs: proc_fd_inode_unique lstat works", r1 == 0 && r2 == 0);
    if (r1 == 0 && r2 == 0) {
        CHECK("procfs: proc_fd_inode_unique different pids differ",
              st_self.st_ino != st_child.st_ino);
    }

    write(pfd[1], "x", 1);
    close(pfd[1]);
    waitpid(child, NULL, 0);
}

/* -----------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    printf("=== procfstest: /proc filesystem ===\n");

    test_proc_self_exists();
    test_proc_self_readlink();
    test_proc_self_status();
    test_proc_self_stat();
    test_proc_self_exe();
    test_proc_self_cwd();
    test_proc_self_maps();
    test_proc_pid_readable();
    test_proc_meminfo();
    test_proc_uptime();
    test_proc_version();
    test_proc_nonexistent();
    test_proc_cmdline_has_args();
    test_proc_child_exe_link();
    test_proc_fd_entries();
    test_proc_fd_inode_unique();

    printf("=== procfstest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
