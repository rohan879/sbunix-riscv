/*
 * exectest.c -- comprehensive execve compliance tests (autograder "06_exec")
 *
 * Uses self-exec with argv[1]="--subtest" to verify child-side behavior
 * after execve (argv, envp, cwd, fd inheritance, signal reset, etc.).
 *
 * Run from the shell:  exectest
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * Helper: read all output from a pipe fd into buf, return total bytes.
 * ---------------------------------------------------------------------- */
static int read_all(int fd, char *buf, int bufsz)
{
    int total = 0;
    while (total < bufsz - 1) {
        ssize_t n = read(fd, buf + total, bufsz - 1 - total);
        if (n <= 0)
            break;
        total += (int)n;
    }
    buf[total] = '\0';
    return total;
}

/* -----------------------------------------------------------------------
 * Helper: fork, redirect child stdout to pipe, exec with given args,
 *         read child output, wait, return exit status via *status.
 *         Returns bytes read into outbuf.
 * ---------------------------------------------------------------------- */
static int fork_exec_capture(const char *path, char *const argv[],
                             char *const envp[],
                             char *outbuf, int outsz, int *status)
{
    int pfd[2];
    if (pipe(pfd) != 0)
        return -1;

    pid_t pid = fork();
    if (pid == 0) {
        /* child: redirect stdout to pipe */
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        if (envp)
            execve(path, argv, envp);
        else {
            char *empty[] = {NULL};
            execve(path, argv, empty);
        }
        _exit(127);
    }

    /* parent */
    close(pfd[1]);
    int nr = read_all(pfd[0], outbuf, outsz);
    close(pfd[0]);
    waitpid(pid, status, 0);
    return nr;
}

/* -----------------------------------------------------------------------
 * Subtest dispatcher: when exectest re-execs itself with --subtest <name>
 * ---------------------------------------------------------------------- */
static int run_subtest(int argc, char *argv[], char *envp[])
{
    if (argc < 3)
        return 1;

    const char *name = argv[2];

    /* --subtest argv_check <expected_argc> <arg1> <arg2> ... */
    if (strcmp(name, "argv_check") == 0) {
        /* Print argc followed by each arg on its own line */
        printf("%d\n", argc - 2); /* subtract --subtest and argv_check */
        for (int i = 2; i < argc; i++)
            printf("%s\n", argv[i]);
        return 0;
    }

    /* --subtest envp_check: print all env vars */
    if (strcmp(name, "envp_check") == 0) {
        if (envp) {
            for (int i = 0; envp[i]; i++)
                printf("%s\n", envp[i]);
        }
        return 0;
    }

    /* --subtest cwd_check: print getcwd */
    if (strcmp(name, "cwd_check") == 0) {
        char buf[256];
        if (getcwd(buf, sizeof(buf)))
            printf("%s\n", buf);
        else
            printf("FAIL\n");
        return 0;
    }

    /* --subtest fd_check <fdnum>: try to read 1 byte from given fd */
    if (strcmp(name, "fd_check") == 0 && argc >= 4) {
        int fd = atoi(argv[3]);
        char buf[1];
        errno = 0;
        ssize_t r = read(fd, buf, 1);
        if (r < 0 && errno == EBADF)
            printf("CLOSED\n");
        else
            printf("OPEN\n");
        return 0;
    }

    /* --subtest fd_open_check <fdnum>: verify fd is open (write to it) */
    if (strcmp(name, "fd_open_check") == 0 && argc >= 4) {
        int fd = atoi(argv[3]);
        errno = 0;
        ssize_t r = write(fd, "X", 1);
        if (r < 0 && errno == EBADF)
            printf("CLOSED\n");
        else
            printf("OPEN\n");
        return 0;
    }

    /* --subtest sig_check: check if SIGUSR1 handler is SIG_DFL */
    if (strcmp(name, "sig_check") == 0) {
        struct sigaction old;
        sigaction(SIGUSR1, (void *)0, &old);
        if (old.sa_handler == SIG_DFL)
            printf("DEFAULT\n");
        else
            printf("CUSTOM\n");
        return 0;
    }

    printf("UNKNOWN_SUBTEST\n");
    return 1;
}

/* -----------------------------------------------------------------------
 * 1. exec_basic — execve("/bin/echo", ["echo","hello"], NULL)
 * ---------------------------------------------------------------------- */
static void test_exec_basic(void)
{
    char *argv[] = {"echo", "hello", NULL};
    char *envp[] = {NULL};
    char buf[128];
    int status;

    fork_exec_capture("/bin/echo", argv, envp, buf, sizeof(buf), &status);
    CHECK("exec_basic: exit 0",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK("exec_basic: output is 'hello'",
          strcmp(buf, "hello\n") == 0);
}

/* -----------------------------------------------------------------------
 * 2. exec_argv_passing — verify argc/argv after exec
 * ---------------------------------------------------------------------- */
static void test_exec_argv_passing(const char *self)
{
    char *argv[] = {(char *)self, "--subtest", "argv_check",
                    "alpha", "beta", "gamma", NULL};
    char *envp[] = {NULL};
    char buf[256];
    int status;

    fork_exec_capture(self, argv, envp, buf, sizeof(buf), &status);
    CHECK("exec_argv_passing: exit 0",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
    /* The child prints: argc-2 = 4 lines: argv_check, alpha, beta, gamma */
    CHECK("exec_argv_passing: correct args",
          strstr(buf, "argv_check\n") != (void *)0 &&
          strstr(buf, "alpha\n") != (void *)0 &&
          strstr(buf, "beta\n") != (void *)0 &&
          strstr(buf, "gamma\n") != (void *)0);
}

/* -----------------------------------------------------------------------
 * 3. exec_envp_passing — verify environment after exec
 * ---------------------------------------------------------------------- */
static void test_exec_envp_passing(const char *self)
{
    char *argv[] = {(char *)self, "--subtest", "envp_check", NULL};
    char *envp[] = {"FOO=bar", "BAZ=qux", NULL};
    char buf[256];
    int status;

    fork_exec_capture(self, argv, envp, buf, sizeof(buf), &status);
    CHECK("exec_envp_passing: exit 0",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK("exec_envp_passing: FOO=bar present",
          strstr(buf, "FOO=bar") != (void *)0);
    CHECK("exec_envp_passing: BAZ=qux present",
          strstr(buf, "BAZ=qux") != (void *)0);
}

/* -----------------------------------------------------------------------
 * 4. exec_enoent — execve("/nonexistent") returns -1, errno=ENOENT
 * ---------------------------------------------------------------------- */
static void test_exec_enoent(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {"nonexistent", NULL};
        char *envp[] = {NULL};
        errno = 0;
        int r = execve("/nonexistent", argv, envp);
        /* execve returned, so it failed */
        if (r == -1 && errno == ENOENT)
            _exit(0);
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK("exec_enoent: execve returns -1 with ENOENT",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* -----------------------------------------------------------------------
 * 5. exec_not_elf — execve on a non-ELF file returns error
 * ---------------------------------------------------------------------- */
static void test_exec_not_elf(void)
{
    /* Create a regular file with non-ELF content (no shebang either) */
    const char *path = "/mnt/not_elf";
    unlink(path);
    int fd = open(path, O_CREAT | O_WRONLY, 0755);
    if (fd < 0) {
        CHECK("exec_not_elf: create test file", 0);
        return;
    }
    write(fd, "NOT AN ELF FILE\n", 16);
    close(fd);

    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {"not_elf", NULL};
        char *envp[] = {NULL};
        errno = 0;
        int r = execve(path, argv, envp);
        /* execve returned, check for ENOEXEC */
        if (r == -1 && (errno == ENOEXEC || errno == EACCES || errno == EINVAL))
            _exit(0);
        /* Any error is acceptable here as long as execve fails */
        if (r == -1)
            _exit(0);
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK("exec_not_elf: execve fails on non-ELF",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
    unlink(path);
}

/* -----------------------------------------------------------------------
 * 6. exec_cloexec — O_CLOEXEC fd is closed after exec
 * ---------------------------------------------------------------------- */
static void test_exec_cloexec(const char *self)
{
    /* Open a pipe; the write end will serve as the test fd */
    int pfd[2];
    if (pipe(pfd) != 0) {
        CHECK("exec_cloexec: pipe()", 0);
        return;
    }
    /* Set O_CLOEXEC on the read end via fcntl */
    int flags = fcntl(pfd[0], F_GETFD);
    fcntl(pfd[0], F_SETFD, flags | FD_CLOEXEC);

    /* Format the fd number as a string for the subtest arg */
    char fdstr[16];
    snprintf(fdstr, sizeof(fdstr), "%d", pfd[0]);

    /* We need to capture the child's stdout, so use another pipe */
    int outpfd[2];
    if (pipe(outpfd) != 0) {
        close(pfd[0]);
        close(pfd[1]);
        CHECK("exec_cloexec: pipe2()", 0);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* child: redirect stdout, exec self --subtest fd_check <fd> */
        close(outpfd[0]);
        close(pfd[1]);
        dup2(outpfd[1], STDOUT_FILENO);
        close(outpfd[1]);
        char *argv[] = {(char *)self, "--subtest", "fd_check", fdstr, NULL};
        char *envp[] = {NULL};
        execve(self, argv, envp);
        _exit(127);
    }
    close(pfd[0]);
    close(pfd[1]);
    close(outpfd[1]);

    char buf[64];
    read_all(outpfd[0], buf, sizeof(buf));
    close(outpfd[0]);

    int status;
    waitpid(pid, &status, 0);
    CHECK("exec_cloexec: fd closed after exec",
          strstr(buf, "CLOSED") != (void *)0);
}

/* -----------------------------------------------------------------------
 * 7. exec_preserves_fd — non-CLOEXEC fd survives exec
 * ---------------------------------------------------------------------- */
static void test_exec_preserves_fd(const char *self)
{
    /* Open a pipe; write end is our test fd (no CLOEXEC) */
    int pfd[2];
    if (pipe(pfd) != 0) {
        CHECK("exec_preserves_fd: pipe()", 0);
        return;
    }

    /* Ensure no CLOEXEC on write end */
    int flags = fcntl(pfd[1], F_GETFD);
    fcntl(pfd[1], F_SETFD, flags & ~FD_CLOEXEC);

    char fdstr[16];
    snprintf(fdstr, sizeof(fdstr), "%d", pfd[1]);

    int outpfd[2];
    if (pipe(outpfd) != 0) {
        close(pfd[0]);
        close(pfd[1]);
        CHECK("exec_preserves_fd: pipe2()", 0);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(outpfd[0]);
        close(pfd[0]);
        dup2(outpfd[1], STDOUT_FILENO);
        close(outpfd[1]);
        char *argv[] = {(char *)self, "--subtest", "fd_open_check", fdstr, NULL};
        char *envp[] = {NULL};
        execve(self, argv, envp);
        _exit(127);
    }
    close(pfd[0]);
    close(pfd[1]);
    close(outpfd[1]);

    char buf[64];
    read_all(outpfd[0], buf, sizeof(buf));
    close(outpfd[0]);

    int status;
    waitpid(pid, &status, 0);
    CHECK("exec_preserves_fd: fd open after exec",
          strstr(buf, "OPEN") != (void *)0);
}

/* -----------------------------------------------------------------------
 * 8. exec_preserves_cwd — cwd unchanged after exec
 * ---------------------------------------------------------------------- */
static void test_exec_preserves_cwd(const char *self)
{
    /* Get current cwd before exec */
    char cwd_before[256];
    if (!getcwd(cwd_before, sizeof(cwd_before))) {
        CHECK("exec_preserves_cwd: getcwd before", 0);
        return;
    }

    char *argv[] = {(char *)self, "--subtest", "cwd_check", NULL};
    char *envp[] = {NULL};
    char buf[256];
    int status;

    fork_exec_capture(self, argv, envp, buf, sizeof(buf), &status);

    /* Remove trailing newline from child output */
    int len = (int)strlen(buf);
    if (len > 0 && buf[len - 1] == '\n')
        buf[len - 1] = '\0';

    CHECK("exec_preserves_cwd: cwd matches",
          strcmp(buf, cwd_before) == 0);
}

/* -----------------------------------------------------------------------
 * 9. exec_resets_signals — custom signal handler reset to SIG_DFL
 * ---------------------------------------------------------------------- */
static void test_exec_resets_signals(const char *self)
{
    /* Install a custom handler for SIGUSR1 in the parent */
    struct sigaction sa;
    sa.sa_handler = SIG_IGN; /* any non-default handler */
    sa.sa_mask = 0;
    sa.sa_flags = 0;
    sa.sa_restorer = 0;
    sigaction(SIGUSR1, &sa, 0);

    char *argv[] = {(char *)self, "--subtest", "sig_check", NULL};
    char *envp[] = {NULL};
    char buf[64];
    int status;

    fork_exec_capture(self, argv, envp, buf, sizeof(buf), &status);

    /* Restore default in parent */
    sa.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &sa, 0);

    CHECK("exec_resets_signals: handler is SIG_DFL after exec",
          strstr(buf, "DEFAULT") != (void *)0);
}

/* -----------------------------------------------------------------------
 * 10. exec_shebang — exec a script with #!/bin/sh shebang
 * ---------------------------------------------------------------------- */
static void test_exec_shebang(void)
{
    const char *path = "/mnt/shebang_test.sh";
    unlink(path);
    int fd = open(path, O_CREAT | O_WRONLY, 0755);
    if (fd < 0) {
        CHECK("exec_shebang: create script", 0);
        return;
    }
    const char *script = "#!/bin/sh\necho shebang_ok\n";
    write(fd, script, strlen(script));
    close(fd);

    char *argv[] = {"shebang_test.sh", NULL};
    char *envp[] = {NULL};
    char buf[128];
    int status;

    fork_exec_capture(path, argv, envp, buf, sizeof(buf), &status);
    CHECK("exec_shebang: exit 0",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK("exec_shebang: output contains 'shebang_ok'",
          strstr(buf, "shebang_ok") != (void *)0);

    unlink(path);
}

/* -----------------------------------------------------------------------
 * 11. exec_relative_path — exec with path relative to cwd
 * ---------------------------------------------------------------------- */
static void test_exec_relative_path(void)
{
    /*
     * chdir to /bin, then exec "./echo" (relative path).
     * We do this in a child to avoid changing the test process cwd.
     */
    pid_t pid = fork();
    if (pid == 0) {
        if (chdir("/bin") != 0)
            _exit(1);
        char *argv[] = {"echo", "relpath_ok", NULL};
        char *envp[] = {NULL};
        execve("./echo", argv, envp);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK("exec_relative_path: exit 0",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* -----------------------------------------------------------------------
 * main — dispatch subtests or run full test suite
 * ---------------------------------------------------------------------- */
int main(int argc, char *argv[], char *envp[])
{
    /* If re-invoked with --subtest, run that subtest and exit */
    if (argc >= 2 && strcmp(argv[1], "--subtest") == 0)
        return run_subtest(argc, argv, envp);

    /* Determine our own binary path for self-exec tests */
    const char *self = "/bin/exectest";

    printf("=== exectest: execve compliance ===\n");

    test_exec_basic();
    test_exec_argv_passing(self);
    test_exec_envp_passing(self);
    test_exec_enoent();
    test_exec_not_elf();
    test_exec_cloexec(self);
    test_exec_preserves_fd(self);
    test_exec_preserves_cwd(self);
    test_exec_resets_signals(self);
    test_exec_shebang();
    test_exec_relative_path();

    printf("=== exectest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
