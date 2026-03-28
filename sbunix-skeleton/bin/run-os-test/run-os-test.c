#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>

#define MAX_PATH 128
#define MAX_TESTS 12000
#define MAX_OUTPUT 4096
#define MANIFEST_FILE "/bin/os-test-manifest"
#define TOTAL_TESTS_IN_SUITE 5304  /* Total .c files in Sortix os-test */

enum test_result {
    RESULT_PASS = 0,
    RESULT_FAIL = 1,
    RESULT_NEUTRAL = 2,
    RESULT_SKIP = 3,
    RESULT_ERROR = 4
};

struct test_info {
    char path[MAX_PATH];
    char name[MAX_PATH];
    char suite[MAX_PATH];
    int uses_expect;
};

static struct test_info tests[MAX_TESTS];
static int total_tests = 0;
static int passed = 0;
static int failed = 0;
static int neutral = 0;
static int skipped = 0;
static int errors = 0;

static void extract_suite_name(const char *path, char *suite) {
    /* Extract suite name from path like /bin/os-test/stdio/printf -> stdio */
    const char *start = strstr(path, "/os-test/");
    if (!start) {
        strcpy(suite, "unknown");
        return;
    }
    start += 9; /* Skip "/os-test/" */
    
    const char *slash = strchr(start, '/');
    if (!slash) {
        strcpy(suite, start);
        return;
    }
    
    size_t len = slash - start;
    if (len >= MAX_PATH) len = MAX_PATH - 1;
    strncpy(suite, start, len);
    suite[len] = '\0';
}

static int check_expect_support(const char *suite) {
    /* Only these top-level suites use expect file validation.
     * All other suites (especially 'include') use exit code validation. */
    if (strcmp(suite, "stdio") == 0 || 
        strcmp(suite, "malloc") == 0 ||
        strcmp(suite, "pty") == 0 ||
        strcmp(suite, "paths") == 0 ||
        strcmp(suite, "process") == 0 ||
        strcmp(suite, "udp") == 0 ||
        strcmp(suite, "signal") == 0 ||
        strcmp(suite, "io") == 0) {
        return 1;
    }
    return 0;
}

static void add_test(const char *path, const char *relative_name) {
    if (total_tests >= MAX_TESTS) return;
    
    strncpy(tests[total_tests].path, path, MAX_PATH - 1);
    tests[total_tests].path[MAX_PATH - 1] = '\0';
    
    strncpy(tests[total_tests].name, relative_name, MAX_PATH - 1);
    tests[total_tests].name[MAX_PATH - 1] = '\0';
    
    extract_suite_name(path, tests[total_tests].suite);
    tests[total_tests].uses_expect = check_expect_support(tests[total_tests].suite);
    
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
    int line_count = 0;
    
    while (fgets(line, sizeof(line), f)) {
        line_count++;
        
        /* Progress indicator every 200 lines */
        if (line_count % 200 == 0) {
            printf("Loaded %d tests...\n", total_tests);
            fflush(stdout);
        }
        
        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        
        /* Remove trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
            len--;
        }
        if (len > 0 && line[len-1] == '\r') {
            line[len-1] = '\0';
            len--;
        }
        
        /* Skip empty lines after trimming */
        if (len == 0) continue;
        
        /* Extract relative name from full path */
        const char *relative_name = line;
        if (strncmp(line, "/bin/os-test/", 13) == 0) {
            relative_name = line + 13;
        }
        
        /* Trust the manifest - it should only contain valid tests */
        add_test(line, relative_name);
    }
    
    fclose(f);
    return 0;
}

static int compare_with_expect_files(const char *suite, const char *test_name, const char *output) {
    char expect_dir[MAX_PATH];
    snprintf(expect_dir, sizeof(expect_dir), "/bin/os-test-expect/%s.expect", suite);
    
    DIR *dir = opendir(expect_dir);
    if (!dir) {
        /* Expect directory missing - this means the suite uses expect files */
        /* but they weren't copied to rootfs. Return SKIP for now. */
        return RESULT_SKIP;
    }
    
    struct dirent *entry;
    int result = RESULT_FAIL; /* Default to fail if no matches */
    
    while ((entry = readdir(dir)) != NULL) {
        /* Check if this expect file matches our test */
        if (strncmp(entry->d_name, test_name, strlen(test_name)) != 0) continue;
        
        char expect_file[MAX_PATH];
        snprintf(expect_file, sizeof(expect_file), "%s/%s", expect_dir, entry->d_name);
        
        FILE *f = fopen(expect_file, "r");
        if (!f) continue;
        
        char expected[MAX_OUTPUT];
        size_t len = fread(expected, 1, sizeof(expected) - 1, f);
        fclose(f);
        expected[len] = '\0';
        
        /* Compare outputs */
        if (strcmp(output, expected) == 0) {
            /* Determine result type based on file suffix */
            if (strstr(entry->d_name, ".unknown")) {
                result = RESULT_NEUTRAL;
            } else {
                result = RESULT_PASS;
            }
            break;
        }
    }
    
    closedir(dir);
    return result;
}

static enum test_result run_test_with_output(const char *path, char *output, size_t output_size) {
    int pipefd[2];
    if (pipe(pipefd) == -1) return RESULT_ERROR;
    
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: redirect stdout/stderr to pipe */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        
        char *argv[] = {(char *)path, NULL};
        execv(path, argv);
        exit(1);
    } else if (pid > 0) {
        /* Parent: read output and wait for child */
        close(pipefd[1]);
        
        size_t pos = 0;
        ssize_t n;
        while (pos < output_size - 1 && (n = read(pipefd[0], output + pos, output_size - 1 - pos)) > 0) {
            pos += n;
        }
        output[pos] = '\0';
        
        close(pipefd[0]);
        
        int status;
        waitpid(pid, &status, 0);
        
        /* If test had non-zero output but still exited normally, append exit code */
        int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (pos > 0 || exit_code >= 2) {
            size_t remaining = output_size - pos - 1;
            if (remaining > 20) {
                snprintf(output + pos, remaining, "exit: %d", exit_code);
            }
        }
        
        return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? RESULT_PASS : RESULT_FAIL;
    }
    
    return RESULT_ERROR;
}

static enum test_result run_test_simple(const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: run the test with output redirected */
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        char *argv[] = {(char *)path, NULL};
        execv(path, argv);
        exit(1);
    } else if (pid > 0) {
        /* Parent: wait for child */
        int status;
        waitpid(pid, &status, 0);
        return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? RESULT_PASS : RESULT_FAIL;
    }
    return RESULT_ERROR;
}

static enum test_result run_test_single(struct test_info *test) {
    if (test->uses_expect) {
        /* Use expect file validation */
        char output[MAX_OUTPUT];
        enum test_result exec_result = run_test_with_output(test->path, output, sizeof(output));
        
        if (exec_result == RESULT_ERROR) return RESULT_ERROR;
        
        /* Compare with expect files regardless of exit code */
        return compare_with_expect_files(test->suite, test->name, output);
    } else {
        /* Use simple exit code validation */
        return run_test_simple(test->path);
    }
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
    
    /* If specific test requested */
    if (argc == 2) {
        char test_path[MAX_PATH];
        snprintf(test_path, sizeof(test_path), "/bin/os-test/%s", argv[1]);
        
        struct stat st;
        if (stat(test_path, &st) != 0 || !(st.st_mode & S_IXUSR)) {
            printf("Test not found: %s\n", argv[1]);
            printf("The test may not have compiled due to missing POSIX features.\n");
            return 1;
        }
        
        /* Create a temporary test info structure */
        struct test_info single_test;
        strncpy(single_test.path, test_path, MAX_PATH - 1);
        single_test.path[MAX_PATH - 1] = '\0';
        strncpy(single_test.name, argv[1], MAX_PATH - 1);
        single_test.name[MAX_PATH - 1] = '\0';
        extract_suite_name(test_path, single_test.suite);
        single_test.uses_expect = check_expect_support(single_test.suite);
        
        printf("Running: %s (%s validation)\n", argv[1], 
               single_test.uses_expect ? "expect file" : "exit code");
        
        enum test_result result = run_test_single(&single_test);
        const char* result_str = (result == RESULT_PASS) ? "PASS" :
                                (result == RESULT_NEUTRAL) ? "NEUTRAL" : "FAIL";
        printf("%s: %s\n", result_str, argv[1]);
        return (result == RESULT_PASS) ? 0 : 1;
    }
    
    /* Discover all tests from manifest */
    printf("Loading tests from manifest...\n");
    fflush(stdout);
    
    if (load_tests_from_manifest() != 0) {
        /* Fallback: check if directory exists for better error message */
        struct stat st;
        if (stat("/bin/os-test", &st) != 0 || !S_ISDIR(st.st_mode)) {
            printf("os-test directory not found: /bin/os-test\n");
        }
        return 1;
    }
    
    printf("Found %d executable tests\n\n", total_tests);
    
    if (total_tests == 0) {
        printf("No executable tests found.\n");
        printf("This may indicate:\n");
        printf("  1. Tests failed to compile due to missing POSIX features\n");
        printf("  2. File permissions issue\n");
        printf("  3. Build system issue\n");
        printf("\nDebug info:\n");
        printf("  /bin/os-test exists: %s\n", (access("/bin/os-test", F_OK) == 0) ? "yes" : "no");
        printf("  /bin/os-test readable: %s\n", (access("/bin/os-test", R_OK) == 0) ? "yes" : "no");
        return 1;
    }
    
    printf("Running %d tests (press Ctrl-C to stop)...\n", total_tests);
    printf("Legend: X=exit code validation, E=expect file validation\n\n");
    
    /* Run all tests */
    for (int i = 0; i < total_tests; i++) {
        enum test_result result = run_test(i);
        const char *result_str;
        char suffix;
        
        /* Determine validation type suffix */
        suffix = tests[i].uses_expect ? 'E' : 'X';
        
        switch (result) {
            case RESULT_PASS:
                result_str = "PASS";
                passed++;
                break;
            case RESULT_FAIL:
                result_str = "FAIL";
                failed++;
                break;
            case RESULT_NEUTRAL:
                result_str = "NEUT";
                neutral++;
                break;
            case RESULT_SKIP:
                result_str = "SKIP";
                skipped++;
                break;
            case RESULT_ERROR:
            default:
                result_str = "ERR ";
                errors++;
                break;
        }
        
        /* Calculate progress and pass rate */
        int progress_pct = (i + 1) * 100 / total_tests;
        int current_success = passed + neutral;
        int current_total = i + 1;
        int pass_rate = current_total > 0 ? current_success * 100 / current_total : 0;
        
        /* Print result: [n/total %% | pass%%] RESULT<suffix>: test/name */
        printf("[%d/%d %d%% | %d%% pass] %s%c: %s\n", 
               i + 1, total_tests, progress_pct, pass_rate,
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
        int success_rate = (passed + neutral) * 100 / total_tests;
        printf("\nPass rate: %d%% (of %d compiled tests)\n", success_rate, total_tests);
    }
    
    printf("\n");
    show_usage();
    
    return (failed > 0) ? 1 : 0;
}