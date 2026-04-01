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
#define MAX_SUITE 32
#define MAX_TESTS 12000
#define MAX_OUTPUT 4096
#define TEST_ROOT "/test"
#define TEST_BIN_ROOT TEST_ROOT "/bin"
#define OS_TEST_BIN_ROOT TEST_ROOT "/bin/os-test"
#define MANIFEST_FILE TEST_ROOT "/os-test-manifest"
#define EXPECT_ROOT TEST_ROOT "/os-test-expect"
#define RESULTS_DIR TEST_ROOT "/var/log/os-test"

#ifndef TOTAL_TESTS_IN_SUITE
#define TOTAL_TESTS_IN_SUITE 5304
#endif

/* Known Sortix outcome strings (compile-time failures from compile.sh) */
static const char *outcome_strings[] = {
    "good", "bad", "unknown", "unrated", "missing_optional",
    "outside_libc", "extension", "previous_posix", "compile_error",
    "incompatible", "missing_header", "pollution", "undeclared",
    "undefined", "unknown_type", NULL
};

enum test_result {
    RESULT_PASS = 0,
    RESULT_FAIL = 1,
    RESULT_NEUTRAL = 2,
    RESULT_SKIP = 3,
    RESULT_ERROR = 4
};

struct test_info {
    char path[MAX_PATH];
    char name[MAX_NAME];
    char suite[MAX_SUITE];
};

static struct test_info tests[MAX_TESTS];
static int total_tests = 0;
static int passed = 0;
static int failed = 0;
static int neutral = 0;
static int skipped = 0;
static int errors = 0;

static volatile sig_atomic_t timed_out = 0;

static void alarm_handler(int sig) {
    (void)sig;
    timed_out = 1;
}

static void extract_suite_name(const char *path, char *suite) {
    const char *start = strstr(path, "/os-test/");
    if (!start) { strcpy(suite, "unknown"); return; }
    start += 9;
    const char *slash = strchr(start, '/');
    if (!slash) {
        strncpy(suite, start, MAX_SUITE - 1);
        suite[MAX_SUITE - 1] = '\0';
        return;
    }
    size_t len = slash - start;
    if (len >= MAX_SUITE) len = MAX_SUITE - 1;
    strncpy(suite, start, len);
    suite[len] = '\0';
}

static void add_test(const char *path, const char *relative_name) {
    if (total_tests >= MAX_TESTS) return;
    if (!relative_name || relative_name[0] == '\0') {
        fprintf(stderr, "WARNING: skipping manifest entry with empty name (path: %s)\n",
                path ? path : "(null)");
        return;
    }
    if (!path || path[0] == '\0') {
        fprintf(stderr, "WARNING: skipping manifest entry with empty path (name: %s)\n",
                relative_name);
        return;
    }

    strncpy(tests[total_tests].path, path, MAX_PATH - 1);
    tests[total_tests].path[MAX_PATH - 1] = '\0';
    strncpy(tests[total_tests].name, relative_name, MAX_NAME - 1);
    tests[total_tests].name[MAX_NAME - 1] = '\0';
    extract_suite_name(path, tests[total_tests].suite);
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
        if (strncmp(line, "/test/bin/os-test/", 18) == 0) rel = line + 18;
        add_test(line, rel);
    }
    fclose(f);
    return 0;
}

/*
 * Strip trailing newline from string, return new length.
 */
static size_t strip_trailing_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r'))
        s[--len] = '\0';
    return len;
}

static const char *test_leaf_name(const char *test_name) {
    const char *slash = strrchr(test_name, '/');
    return slash ? slash + 1 : test_name;
}

static size_t expect_entry_match_len(const char *entry_name,
                                     const char *test_name) {
    size_t tlen = strlen(test_name);
    if (strncmp(entry_name, test_name, tlen) == 0 && entry_name[tlen] == '.')
        return tlen;

    const char *leaf = test_leaf_name(test_name);
    size_t llen = strlen(leaf);
    if (strncmp(entry_name, leaf, llen) == 0 && entry_name[llen] == '.')
        return llen;

    return 0;
}

/*
 * Check if content is a known Sortix outcome string.
 * The compare is done against a newline-stripped copy of content.
 */
static const char *classify_known_outcome(char *content) {
    char stripped[MAX_OUTPUT];
    strncpy(stripped, content, sizeof(stripped) - 1);
    stripped[sizeof(stripped) - 1] = '\0';
    strip_trailing_newline(stripped);
    for (int i = 0; outcome_strings[i]; i++) {
        if (strcmp(stripped, outcome_strings[i]) == 0)
            return outcome_strings[i];
    }
    return NULL;
}

/*
 * Classify test outcome from .out content, following Sortix html.c logic:
 *
 * 1. Strip trailing newline from content
 * 2. Check if stripped content is a known outcome string
 *    -> GOOD/BAD/SKIP/NEUTRAL
 * 3. Try the suite's .expect directory — scan for expect files matching this
 *    specific test (e.g., test.posix, test.posix.0, test.1, test.2)
 *    -> GOOD if any variant matches, UNKNOWN if match has .unknown. prefix
 *    -> If no matching expect files found, fall through to step 4
 * 4. Compare ORIGINAL .out content (with newline) to "exit: 0\n"
 *    -> GOOD (PASS) or BAD (FAIL)
 */
static enum test_result classify_from_out(const char *test_name,
                                           const char *suite,
                                           char *out_content) {
    /* 1+2. Check for known outcome strings */
    const char *known = classify_known_outcome(out_content);
    if (known) {
        if (strcmp(known, "good") == 0) return RESULT_PASS;
        if (strcmp(known, "missing_optional") == 0) return RESULT_SKIP;
        if (strcmp(known, "unknown") == 0) return RESULT_NEUTRAL;
        return RESULT_FAIL;
    }

    /* 3. Try expect directory — check for individual test expect files */
    {
        char expect_dir[MAX_PATH];
        snprintf(expect_dir, sizeof(expect_dir),
                 EXPECT_ROOT "/%s.expect", suite);
        DIR *dir = opendir(expect_dir);
        if (dir) {
            /* Strip newline from output for expect comparison
             * (expect files don't have trailing newlines) */
            char stripped[MAX_OUTPUT];
            strncpy(stripped, out_content, sizeof(stripped) - 1);
            stripped[sizeof(stripped) - 1] = '\0';
            strip_trailing_newline(stripped);

            struct dirent *entry;
            int found_expectation = 0;
            int result = RESULT_FAIL;

            while ((entry = readdir(dir)) != NULL) {
                size_t match_len = expect_entry_match_len(entry->d_name,
                                                          test_name);
                if (!match_len)
                    continue;

                found_expectation = 1;

                char expect_file[MAX_PATH];
                snprintf(expect_file, sizeof(expect_file),
                         "%s/%s", expect_dir, entry->d_name);
                FILE *ef = fopen(expect_file, "r");
                if (!ef) continue;
                char expected[MAX_OUTPUT];
                size_t elen = fread(expected, 1, sizeof(expected) - 1, ef);
                fclose(ef);
                expected[elen] = '\0';
                strip_trailing_newline(expected);

                if (strcmp(stripped, expected) == 0) {
                    if (strstr(entry->d_name + match_len + 1, "unknown"))
                        result = RESULT_NEUTRAL;
                    else
                        result = RESULT_PASS;
                    break;
                }
            }
            closedir(dir);
            if (found_expectation)
                return result;
        }
    }

    /* 4. No expect files found — compare .out content to "exit: 0\n" */
    if (strcmp(out_content, "exit: 0\n") == 0)
        return RESULT_PASS;
    return RESULT_FAIL;
}

/*
 * Run a test, capturing stdout+stderr to a buffer.
 * Follows Sortix run.sh: ./$1 > $2 2>&1
 * Appends "exit: CODE\n" if output is empty OR exit code >= 2.
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

        /*
         * Sortix run.sh:
         *   if [ ! -s $2 ] || [ 2 -le $CODE ]; then
         *     echo "exit: $CODE" >> $2
         *   fi
         */
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
 * Path: RESULTS_DIR/<name>.out  (e.g., /test/var/log/os-test/include/errno/ENFILE.out)
 */
static void write_out_file(const char *name, const char *content) {
    char out_path[MAX_PATH];
    snprintf(out_path, sizeof(out_path), "%s/%s.out", RESULTS_DIR, name);
    mkdirs_for_path(out_path);
    FILE *f = fopen(out_path, "w");
    if (f) { fputs(content, f); fclose(f); }
}

/*
 * Run a single test and classify its result.
 * Flow matching Sortix:
 * 1. Run test, capture output (stdout+stderr)
 * 2. Append "exit: CODE\n" if empty or exit>=2 (run.sh behavior)
 * 3. Write .out file (Sortix artifact for html.c)
 * 4. Classify from .out content
 */
static enum test_result run_test_single(struct test_info *test) {
    char output[MAX_OUTPUT];
    struct sigaction old_alrm;
    struct sigaction sa;
    timed_out = 0;
    sa.sa_handler = alarm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_restorer = NULL;
    sigaction(SIGALRM, &sa, &old_alrm);
    alarm(5);
    run_test_capture(test->path, output, sizeof(output));
    alarm(0);
    sigaction(SIGALRM, &old_alrm, NULL);

    if (timed_out) {
        snprintf(output, sizeof(output), "exit: 124\n");
    }

    write_out_file(test->name, output);
    return classify_from_out(test->name, test->suite, output);
}

static enum test_result run_test(int test_idx) {
    return run_test_single(&tests[test_idx]);
}

static void show_usage(void) {
    printf("Usage:\n");
    printf("  run-os-test                 - Run all tests\n");
    printf("  run-os-test <test-name>     - Run specific test\n");
    printf("\n");
    printf("Examples:\n");
    printf("  run-os-test include/stdarg/va_arg\n");
    printf("  run-os-test include/stdio/printf\n");
}

int main(int argc, char *argv[]) {
    printf("=== Sortix os-test Suite ===\n\n");
    mkdir(RESULTS_DIR, 0755);

    if (argc == 2) {
        char test_path[MAX_PATH];
        snprintf(test_path, sizeof(test_path), OS_TEST_BIN_ROOT "/%s", argv[1]);
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
        extract_suite_name(test_path, single_test.suite);

        printf("Running: %s\n", argv[1]);
        enum test_result result = run_test_single(&single_test);
        const char* rstr = (result == RESULT_PASS) ? "PASS" :
                           (result == RESULT_NEUTRAL) ? "NEUTRAL" :
                           (result == RESULT_SKIP) ? "SKIP" : "FAIL";
        printf("%s: %s\n", rstr, argv[1]);
        return (result == RESULT_PASS) ? 0 : 1;
    }

    printf("Loading tests from manifest...\n");
    fflush(stdout);
    if (load_tests_from_manifest() != 0) {
        struct stat st;
        if (stat(OS_TEST_BIN_ROOT, &st) != 0 || !S_ISDIR(st.st_mode))
            printf("os-test directory not found: %s\n", OS_TEST_BIN_ROOT);
        return 1;
    }

    printf("Found %d executable tests\n\n", total_tests);
    if (total_tests == 0) {
        printf("No executable tests found.\n");
        return 1;
    }

    printf("Running %d tests (press Ctrl-C to stop)...\n", total_tests);
    printf("Legend: X=exit code, E=expect file\n\n");

    for (int i = 0; i < total_tests; i++) {
        if (tests[i].name[0] == '\0') {
            printf("[%d/%d] ERROR: test at index %d has blank name (path: %s)\n",
                   i + 1, total_tests, i, tests[i].path);
            errors++;
            continue;
        }

        enum test_result result = run_test(i);

        /* Determine suffix: 'E' if test has expect files, 'X' otherwise */
        char suffix = 'X';
        {
            char expect_dir[MAX_PATH];
            snprintf(expect_dir, sizeof(expect_dir),
                     EXPECT_ROOT "/%s.expect", tests[i].suite);
            DIR *edir = opendir(expect_dir);
            if (edir) {
                struct dirent *ent;
                while ((ent = readdir(edir)) != NULL) {
                    if (expect_entry_match_len(ent->d_name, tests[i].name)) {
                        suffix = 'E';
                        break;
                    }
                }
                closedir(edir);
            }
        }

        const char *result_str;
        switch (result) {
            case RESULT_PASS:    result_str = "PASS"; passed++; break;
            case RESULT_FAIL:    result_str = "FAIL"; failed++; break;
            case RESULT_NEUTRAL: result_str = "NEUT"; neutral++; break;
            case RESULT_SKIP:    result_str = "SKIP"; skipped++; break;
            case RESULT_ERROR:
            default:             result_str = "ERR "; errors++; break;
        }

        int pct = (i + 1) * 100 / total_tests;
        int rate = (i + 1) > 0 ? (passed + neutral) * 100 / (i + 1) : 0;

        printf("[%d/%d %d%% | %d%% pass] %s%c: %s\n",
               i + 1, total_tests, pct, rate,
               result_str, suffix, tests[i].name);
    }

    printf("\n=== Final Results ===\n");
    int compile_skipped = TOTAL_TESTS_IN_SUITE - total_tests;
    int compile_pct = total_tests * 100 / TOTAL_TESTS_IN_SUITE;
    printf("Tests in suite:  %d\n", TOTAL_TESTS_IN_SUITE);
    printf("Compiled:        %d (%d%%)\n", total_tests, compile_pct);
    printf("Build skipped:   %d (missing POSIX features)\n", compile_skipped);
    printf("\n");
    printf("Results (of compiled tests):\n");
    printf("  Passed:   %d\n", passed);
    printf("  Failed:   %d\n", failed);
    printf("  Neutral:  %d\n", neutral);
    printf("  Skipped:  %d\n", skipped);
    printf("  Errors:   %d\n", errors);

    if (total_tests > 0) {
        int sr = (passed + neutral) * 100 / total_tests;
        printf("\nPass rate: %d%% (of %d compiled tests)\n",
               sr, total_tests);
    }
    printf("\n");
    show_usage();

    return (failed > 0) ? 1 : 0;
}
