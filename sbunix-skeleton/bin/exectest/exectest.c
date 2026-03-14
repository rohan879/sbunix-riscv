#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/mman.h>

static int tests_passed = 0;
static int tests_failed = 0;

static void check(int cond, const char *name) {
    if (cond) { printf("[PASS] exec: %s\n", name); tests_passed++; }
    else { printf("[FAIL] exec: %s\n", name); tests_failed++; }
}

static void check_output(int cond, const char *name, const char *actual) {
    if (cond) {
        printf("[PASS] exec: %s\n", name);
        tests_passed++;
        return;
    }
    printf("[FAIL] exec: %s\n", name);
    printf("[INFO] exec actual output: %s\n", actual);
    tests_failed++;
}

static int matches_output(const char *actual, const char *expected) {
    size_t expected_len = strlen(expected);
    if (strcmp(actual, expected) == 0)
        return 1;
    if (strncmp(actual, expected, expected_len) == 0 &&
        strcmp(actual + expected_len, "\n") == 0)
        return 1;
    return 0;
}

static int run_and_capture(char *const argv[], char *const envp[],
                           char *buf, size_t buflen, int *status_out) {
    int pfd[2];
    pid_t pid;
    ssize_t total = 0;

    if (pipe(pfd) != 0)
        return -1;

    pid = fork();
    if (pid == 0) {
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        execve(argv[0], argv, envp);
        _exit(127);
    }

    close(pfd[1]);
    if (buflen > 0) {
        while (total < (ssize_t)buflen - 1) {
            ssize_t nread = read(pfd[0], buf + total, buflen - 1 - (size_t)total);
            if (nread <= 0)
                break;
            total += nread;
        }
        buf[total] = '\0';
    }
    close(pfd[0]);
    waitpid(pid, status_out, 0);
    return (int)total;
}

int main(void) {
    int status;
    pid_t pid;
    char outbuf[128];

    /* execve echo */
    {
        char *argv[] = {"/bin/echo", "exec_ok", NULL};
        char *envp[] = {NULL};
        memset(outbuf, 0, sizeof(outbuf));
        run_and_capture(argv, envp, outbuf, sizeof(outbuf), &status);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
              strcmp(outbuf, "exec_ok\n") == 0,
              "execve echo");
    }

    /* execve nonexistent */
    pid = fork();
    if (pid == 0) {
        char *argv[] = {"/bin/nonexistent", NULL};
        char *envp[] = {NULL};
        int r = execve("/bin/nonexistent", argv, envp);
        _exit(r == -1 ? 0 : 1);
    }
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "execve nonexistent fails");

    /* execve with multiple args */
    {
        char *argv[] = {"/bin/argvdump", "a", "b", "c", NULL};
        char *envp[] = {NULL};
        memset(outbuf, 0, sizeof(outbuf));
        run_and_capture(argv, envp, outbuf, sizeof(outbuf), &status);
        check_output(WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                     matches_output(outbuf,
                                    "argc=4\n"
                                    "argv[0]=/bin/argvdump\n"
                                    "argv[1]=a\n"
                                    "argv[2]=b\n"
                                    "argv[3]=c\n"
                                    "env.SBX_EXEC_ENV=\n"),
                     "execve preserved argv order", outbuf);
    }

    /* execve preserves environment entries */
    {
        char *argv[] = {"/bin/argvdump", "envcheck", NULL};
        char *envp[] = {"SBX_EXEC_ENV=exec_env", NULL};
        memset(outbuf, 0, sizeof(outbuf));
        run_and_capture(argv, envp, outbuf, sizeof(outbuf), &status);
        check_output(WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                     matches_output(outbuf,
                                    "argc=2\n"
                                    "argv[0]=/bin/argvdump\n"
                                    "argv[1]=envcheck\n"
                                    "env.SBX_EXEC_ENV=exec_env\n"),
                     "execve preserved custom environment", outbuf);
    }

    /* execve can be used after creating a writable script */
    {
        int fd = open("/mnt/exec_script.sh", O_WRONLY | O_CREAT | O_TRUNC, 0755);
        if (fd < 0) {
            check(0, "create writable script");
        } else {
            write(fd, "echo script_exec_ok\n", 20);
            close(fd);
            char *argv[] = {"/bin/sh", "/mnt/exec_script.sh", NULL};
            char *envp[] = {NULL};
            memset(outbuf, 0, sizeof(outbuf));
            run_and_capture(argv, envp, outbuf, sizeof(outbuf), &status);
            check(WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  strcmp(outbuf, "script_exec_ok\n") == 0,
                  "execve writable script");
        }
    }
    unlink("/mnt/exec_script.sh");

    /* failed execve should leave the current image usable */
    pid = fork();
    if (pid == 0) {
        void *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
        if (map == MAP_FAILED)
            _exit(2);
        ((char *)map)[0] = 'a';
        if (execve("/bin/nonexistent", (char *const[]){"/bin/nonexistent", NULL},
                   (char *const[]){NULL}) != -1)
            _exit(3);
        if (munmap(map, 4096) != 0)
            _exit(4);
        map = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
        if (map == MAP_FAILED)
            _exit(5);
        ((char *)map)[0] = 'b';
        munmap(map, 4096);
        _exit(0);
    }
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "execve failure preserved process mappings");

    printf("exectest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
