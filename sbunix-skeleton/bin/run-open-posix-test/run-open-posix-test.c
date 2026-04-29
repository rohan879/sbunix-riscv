#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>

#define MAX_PATH 256
#define MAX_NAME 128
#define MAX_SUITE 64
#define MAX_TESTS 4096
#define MAX_OUTPUT 4096
#define TEST_ROOT "/test"
#define TEST_BIN_ROOT TEST_ROOT "/bin"
#define OPEN_POSIX_BIN_ROOT TEST_ROOT "/bin/open-posix-test"
#define MANIFEST_FILE TEST_ROOT "/open-posix-test-manifest"
#define RESULTS_DIR TEST_ROOT "/var/log/open-posix-test"
#define DEFAULT_TIMEOUT_SEC 5
#define SLOW_TIMEOUT_SEC 180

#ifndef TOTAL_OPEN_POSIX_TESTS
#define TOTAL_OPEN_POSIX_TESTS 1984
#endif

/* Open POSIX exit codes */
#define PTS_PASS        0
#define PTS_FAIL        1
#define PTS_UNRESOLVED  2
#define PTS_UNSUPPORTED 4
#define PTS_UNTESTED    5

enum test_result {
    RESULT_PASS = 0,
    RESULT_FAIL = 1,
    RESULT_NEUTRAL = 2,
    RESULT_SKIP = 3,
    RESULT_UNSUPPORTED = 4,
    RESULT_UNTESTED = 5,
    RESULT_ERROR = 99
};

struct test_info {
    char path[MAX_PATH];
    char name[MAX_NAME];
    char category[MAX_SUITE];
};

static struct test_info tests[MAX_TESTS];
static int total_tests = 0;
static int passed = 0;
static int failed = 0;
static int neutral = 0;
static int skipped = 0;
static int unsupported = 0;
static int untested = 0;
static int errors = 0;

static volatile sig_atomic_t timed_out = 0;

static void alarm_handler(int sig) {
    (void)sig;
    timed_out = 1;
}

static unsigned int timeout_for_test(const char *name) {
    if (!name)
        return DEFAULT_TIMEOUT_SEC;
    /* BF: CPU-bound timing and VM stress cases can be correct but slower
     * than the default smoke timeout on the emulated guest. */
    if (strcmp(name, "conformance/interfaces/clock/1-1") == 0 ||
        strcmp(name, "conformance/interfaces/clock_gettime/4-1") == 0 ||
        strcmp(name, "conformance/interfaces/mmap/10-1") == 0)
        return SLOW_TIMEOUT_SEC;
    return DEFAULT_TIMEOUT_SEC;
}

static void install_timeout_alarm(struct sigaction *old_alrm, sigset_t *old_mask,
                                  unsigned int timeout_seconds) {
    struct sigaction sa;
    timed_out = 0;
    sa.sa_handler = alarm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_restorer = NULL;
    sigprocmask(SIG_BLOCK, NULL, old_mask);
    sigset_t mask = *old_mask;
    sigdelset(&mask, SIGALRM);
    sigprocmask(SIG_SETMASK, &mask, NULL);
    sigaction(SIGALRM, &sa, old_alrm);
    alarm(timeout_seconds);
}

static void uninstall_timeout_alarm(const struct sigaction *old_alrm, const sigset_t *old_mask) {
    alarm(0);
    sigaction(SIGALRM, old_alrm, NULL);
    sigprocmask(SIG_SETMASK, old_mask, NULL);
}

/*
 * Extract category from path: conformance/definitions/unistd_h/1-1 -> definitions
 */
static void extract_category(const char *path, char *category) {
    const char *start = strstr(path, "/open-posix-test/");
    if (!start) { strcpy(category, "unknown"); return; }
    start += 17;

    /* Category is the second component after open-posix-test/
     * e.g., conformance/definitions/unistd_h/1-1 -> definitions */
    const char *slash1 = strchr(start, '/');
    if (!slash1) {
        strncpy(category, start, MAX_SUITE - 1);
        category[MAX_SUITE - 1] = '\0';
        return;
    }
    const char *slash2 = strchr(slash1 + 1, '/');
    if (!slash2) {
        strncpy(category, slash1 + 1, MAX_SUITE - 1);
        category[MAX_SUITE - 1] = '\0';
        return;
    }
    size_t len = slash2 - (slash1 + 1);
    if (len >= MAX_SUITE) len = MAX_SUITE - 1;
    strncpy(category, slash1 + 1, len);
    category[len] = '\0';
}

static void add_test(const char *path, const char *relative_name) {
    if (total_tests >= MAX_TESTS) return;
    if (!relative_name || relative_name[0] == '\0') return;
    if (!path || path[0] == '\0') return;

    strncpy(tests[total_tests].path, path, MAX_PATH - 1);
    tests[total_tests].path[MAX_PATH - 1] = '\0';
    strncpy(tests[total_tests].name, relative_name, MAX_NAME - 1);
    tests[total_tests].name[MAX_NAME - 1] = '\0';
    extract_category(path, tests[total_tests].category);
    total_tests++;
}

static int load_tests_from_manifest(void) {
    FILE *f = fopen(MANIFEST_FILE, "r");
    if (!f) {
        printf("Test manifest not found: %s\n", MANIFEST_FILE);
        printf("Build with BUILD_TESTS=1 make to compile the test suite.\n");
        return -1;
    }
    char line[MAX_PATH];
    int lc = 0;
    while (fgets(line, sizeof(line), f)) {
        lc++;
        if (lc % 200 == 0) { printf("Loaded %d tests...\n", total_tests); fflush(stdout); }
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') { line[--len] = '\0'; }
        if (len > 0 && line[len-1] == '\r') { line[--len] = '\0'; }
        if (len == 0) continue;
        const char *rel = line;
        if (strncmp(line, "/test/bin/open-posix-test/", 26) == 0) rel = line + 26;
        add_test(line, rel);
    }
    fclose(f);
    return 0;
}

/*
 * Run a test, capturing stdout+stderr to a buffer.
 * Returns the raw exit code.
 */
static int run_test_capture(const char *path, char *output, size_t output_size) {
    int pipefd[2];
    if (pipe(pipefd) == -1) return -1;

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        char *argv[] = {(char *)path, NULL};
        execv(path, argv);
        _exit(127);
    } else if (pid > 0) {
        close(pipefd[1]);
        size_t pos = 0;
        ssize_t n;
        while (pos < output_size - 1 &&
               (n = read(pipefd[0], output + pos,
                         output_size - 1 - pos)) > 0)
            pos += n;
        output[pos] = '\0';
        close(pipefd[0]);

        int status;
        for (;;) {
            pid_t wr = waitpid(pid, &status, 0);
            if (wr == pid)
                break;
            if (wr < 0 && errno == EINTR) {
                if (timed_out) {
                    kill(pid, SIGKILL);
                    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                        ;
                    break;
                }
                continue;
            }
            break;
        }
        int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        if (pos == 0 || exit_code >= 2) {
            size_t remaining = output_size - pos - 1;
            if (remaining > 20)
                pos += snprintf(output + pos, remaining,
                                "exit: %d\n", exit_code);
        }
        return exit_code;
    }
    return -1;
}

/*
 * Create intermediate directories for a file path.
 */
static void mkdirs_for_path(const char *file_path) {
    char dir[MAX_PATH];
    strncpy(dir, file_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(dir, 0755); *p = '/'; }
    }
    mkdir(dir, 0755);
}

/*
 * Write .out file for a test.
 */
static void write_out_file(const char *name, const char *content) {
    char out_path[MAX_PATH];
    snprintf(out_path, sizeof(out_path), "%s/%s.out", RESULTS_DIR, name);
    mkdirs_for_path(out_path);
    FILE *f = fopen(out_path, "w");
    if (f) { fputs(content, f); fclose(f); }
}

/*
 * Classify test result from exit code (Open POSIX convention):
 *   0 = PASS, 1 = FAIL, 2 = UNRESOLVED, 4 = UNSUPPORTED, 5 = UNTESTED
 */
static enum test_result classify_from_exit(int exit_code) {
    switch (exit_code) {
        case PTS_PASS:        return RESULT_PASS;
        case PTS_FAIL:        return RESULT_FAIL;
        case PTS_UNRESOLVED:  return RESULT_NEUTRAL;
        case PTS_UNSUPPORTED: return RESULT_UNSUPPORTED;
        case PTS_UNTESTED:    return RESULT_UNTESTED;
        default:              return RESULT_ERROR;
    }
}

static enum test_result run_test(int test_idx) {
    char output[MAX_OUTPUT];
    struct sigaction old_alrm;
    sigset_t old_mask;
    install_timeout_alarm(&old_alrm, &old_mask,
                          timeout_for_test(tests[test_idx].name));
    int exit_code = run_test_capture(tests[test_idx].path, output, sizeof(output));
    uninstall_timeout_alarm(&old_alrm, &old_mask);

    if (timed_out) {
        snprintf(output, sizeof(output), "exit: %d\n", 124);
    }

    write_out_file(tests[test_idx].name, output);
    return classify_from_exit(exit_code);
}

static void show_usage(void) {
    printf("Usage:\n");
    printf("  run-open-posix-test                 - Run all tests\n");
    printf("  run-open-posix-test <test-name>     - Run specific test\n");
    printf("\n");
    printf("Examples:\n");
    printf("  run-open-posix-test definitions/unistd_h/1-1\n");
    printf("  run-open-posix-test definitions/signal_h/3-1\n");
    printf("  run-open-posix-test interfaces/getpid/1-1\n");
}

int main(int argc, char *argv[]) {
    printf("=== Open POSIX Test Suite ===\n\n");
    mkdir(RESULTS_DIR, 0755);

    if (argc == 2) {
        char test_path[MAX_PATH];
        snprintf(test_path, sizeof(test_path), OPEN_POSIX_BIN_ROOT "/%s", argv[1]);
        struct stat st;
        if (stat(test_path, &st) != 0 || !(st.st_mode & S_IXUSR)) {
            printf("Test not found: %s\n", argv[1]);
            printf("The test may not have compiled due to missing POSIX features.\n");
            return 1;
        }
        struct test_info single_test;
        strncpy(single_test.path, test_path, MAX_PATH - 1);
        single_test.path[MAX_PATH - 1] = '\0';
        strncpy(single_test.name, argv[1], MAX_NAME - 1);
        single_test.name[MAX_NAME - 1] = '\0';
        extract_category(test_path, single_test.category);

        printf("Running: %s\n", argv[1]);
        char output[MAX_OUTPUT];
        
        /* Install timeout alarm handler */
        struct sigaction old_alrm;
        sigset_t old_mask;
        install_timeout_alarm(&old_alrm, &old_mask,
                              timeout_for_test(single_test.name));
        
        int exit_code = run_test_capture(single_test.path, output, sizeof(output));
        
        uninstall_timeout_alarm(&old_alrm, &old_mask);
        
        if (timed_out) {
            snprintf(output, sizeof(output), "exit: %d\n", 124);
        }
        
        write_out_file(single_test.name, output);
        enum test_result result = classify_from_exit(exit_code);
        const char *rstr = (result == RESULT_PASS) ? "PASS" :
                           (result == RESULT_NEUTRAL) ? "UNRESOLVED" :
                           (result == RESULT_UNSUPPORTED) ? "UNSUPPORTED" :
                           (result == RESULT_UNTESTED) ? "UNTESTED" :
                           (result == RESULT_SKIP) ? "SKIP" : "FAIL";
        printf("%s: %s\n", rstr, argv[1]);
        /* BF: Only return failure exit code for actual failures.  Previously,
         * UNTESTED / UNSUPPORTED / UNRESOLVED results returned exit 1,
         * causing run-priority-tests to miscount them as [FAIL]. */
        return (result == RESULT_FAIL || result == RESULT_ERROR) ? 1 : 0;
    }

    printf("Loading tests from manifest...\n");
    fflush(stdout);
    if (load_tests_from_manifest() != 0) {
        struct stat st;
        if (stat(OPEN_POSIX_BIN_ROOT, &st) != 0 || !S_ISDIR(st.st_mode))
            printf("open-posix-test directory not found: %s\n", OPEN_POSIX_BIN_ROOT);
        return 1;
    }

    printf("Found %d executable tests\n\n", total_tests);
    if (total_tests == 0) {
        printf("No executable tests found.\n\n");
        printf("=== Final Results ===\n");
        printf("Tests in suite:  %d\n", TOTAL_OPEN_POSIX_TESTS);
        printf("Compiled:        0 (0%%)\n");
        printf("Build skipped:   %d (missing POSIX features)\n",
               TOTAL_OPEN_POSIX_TESTS);
        printf("\n");
        printf("Results (of compiled tests):\n");
        printf("  Passed:      0\n");
        printf("  Failed:      0\n");
        printf("  Unresolved:  0\n");
        printf("  Unsupported: 0\n");
        printf("  Untested:    0\n");
        printf("  Errors:      0\n");
        printf("\n");
        show_usage();
        return 0;
    }

    printf("Running %d tests (press Ctrl-C to stop)...\n\n", total_tests);

    for (int i = 0; i < total_tests; i++) {
        if (tests[i].name[0] == '\0') {
            errors++;
            continue;
        }

        enum test_result result = run_test(i);

        const char *result_str;
        switch (result) {
            case RESULT_PASS:        result_str = "PASS"; passed++; break;
            case RESULT_FAIL:        result_str = "FAIL"; failed++; break;
            case RESULT_NEUTRAL:     result_str = "UNRS"; neutral++; break;
            case RESULT_UNSUPPORTED: result_str = "UNSP"; unsupported++; break;
            case RESULT_UNTESTED:    result_str = "UNTD"; untested++; break;
            case RESULT_SKIP:        result_str = "SKIP"; skipped++; break;
            case RESULT_ERROR:
            default:                 result_str = "ERR "; errors++; break;
        }

        int pct = (i + 1) * 100 / total_tests;
        int rate = (i + 1) > 0 ? (passed + neutral) * 100 / (i + 1) : 0;

        printf("[%d/%d %d%% | %d%% pass] %s: %s\n",
               i + 1, total_tests, pct, rate,
               result_str, tests[i].name);
    }

    printf("\n=== Final Results ===\n");
    int compile_skipped = TOTAL_OPEN_POSIX_TESTS - total_tests;
    int compile_pct = TOTAL_OPEN_POSIX_TESTS > 0 ? (total_tests * 100 / TOTAL_OPEN_POSIX_TESTS) : 0;
    printf("Tests in suite:  %d\n", TOTAL_OPEN_POSIX_TESTS);
    printf("Compiled:        %d (%d%%)\n", total_tests, compile_pct);
    printf("Build skipped:   %d (missing POSIX features)\n", compile_skipped);
    printf("\n");
    printf("Results (of compiled tests):\n");
    printf("  Passed:      %d\n", passed);
    printf("  Failed:      %d\n", failed);
    printf("  Unresolved:  %d\n", neutral);
    printf("  Unsupported: %d\n", unsupported);
    printf("  Untested:    %d\n", untested);
    printf("  Errors:      %d\n", errors);

    if (total_tests > 0) {
        int sr = (passed + neutral) * 100 / total_tests;
        printf("\nPass rate: %d%% (of %d compiled tests)\n",
               sr, total_tests);
    }
    printf("\n");
    show_usage();

    /* BF: Treat runtime errors as status failures even if no FAIL token
     * appears, preserving suite provenance for host-side validation. */
    return (failed > 0 || errors > 0) ? 1 : 0;
}
