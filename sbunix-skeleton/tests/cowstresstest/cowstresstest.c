/*
 * cowstresstest.c -- stress test COW fork, exec, exit, and page table cleanup.
 *
 * Probes the exact code paths that caused intermittent failures:
 * 1. Rapid fork+exit cycles (stresses freewalk_level page table cleanup)
 * 2. Fork+exec+exit cycles (stresses exec page table replacement + cleanup)
 * 3. Fork with file I/O on ramfs (stresses COW + VFS interaction)
 * 4. Nested fork chains (stresses deep COW page table trees)
 * 5. Fork+pipe+exit (stresses pipe fd cleanup during process exit)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * 1. Rapid fork+exit: 20 cycles of fork -> _exit(42) -> waitpid
 *    Stresses freewalk_level on every child exit.
 * ---------------------------------------------------------------------- */
static void test_rapid_fork_exit(void)
{
    int ok = 1;
    for (int i = 0; i < 20; i++) {
        pid_t pid = fork();
        if (pid < 0) { ok = 0; break; }
        if (pid == 0) _exit(42);
        int status;
        pid_t w = waitpid(pid, &status, 0);
        if (w != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 42) {
            ok = 0;
            break;
        }
    }
    CHECK("cow_stress: 20 rapid fork+exit cycles", ok);
}

/* -----------------------------------------------------------------------
 * 2. Fork+exec+exit: 10 cycles of fork -> exec(/bin/true) -> waitpid
 *    Stresses exec's old page table freeing + new page table creation.
 * ---------------------------------------------------------------------- */
static void test_fork_exec_exit(void)
{
    int ok = 1;
    for (int i = 0; i < 10; i++) {
        pid_t pid = fork();
        if (pid < 0) { ok = 0; break; }
        if (pid == 0) {
            char *argv[] = {"true", NULL};
            char *envp[] = {NULL};
            execve("/bin/true", argv, envp);
            _exit(127);
        }
        int status;
        pid_t w = waitpid(pid, &status, 0);
        if (w != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            ok = 0;
            break;
        }
    }
    CHECK("cow_stress: 10 fork+exec+exit cycles", ok);
}

/* -----------------------------------------------------------------------
 * 3. Fork with ext2 file I/O: child creates, writes, reads file on /mnt
 *    Stresses COW page table + VFS interaction during child cleanup.
 * ---------------------------------------------------------------------- */
static void test_fork_ext2_io(void)
{
    int ok = 1;
    for (int i = 0; i < 5; i++) {
        pid_t pid = fork();
        if (pid < 0) { ok = 0; break; }
        if (pid == 0) {
            char path[64];
            snprintf(path, sizeof(path), "/tmp/cowtest_%d", i);
            int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd < 0) _exit(1);
            write(fd, "test_data\n", 10);
            close(fd);
            /* Read back */
            fd = open(path, O_RDONLY);
            if (fd < 0) _exit(2);
            char buf[16];
            ssize_t n = read(fd, buf, sizeof(buf));
            close(fd);
            unlink(path);
            if (n != 10 || memcmp(buf, "test_data\n", 10) != 0)
                _exit(3);
            _exit(0);
        }
        int status;
        pid_t w = waitpid(pid, &status, 0);
        if (w != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            ok = 0;
            break;
        }
    }
    CHECK("cow_stress: 5 fork+file_io cycles", ok);
}

/* -----------------------------------------------------------------------
 * 4. Nested fork chain: parent -> child -> grandchild -> great-grandchild
 *    Each level exits with a distinct code. Stresses deep COW trees.
 * ---------------------------------------------------------------------- */
static void test_nested_fork(void)
{
    pid_t pid = fork();
    if (pid < 0) { CHECK("cow_stress: nested fork depth 3", 0); return; }
    if (pid == 0) {
        /* child */
        pid_t pid2 = fork();
        if (pid2 < 0) _exit(10);
        if (pid2 == 0) {
            /* grandchild */
            pid_t pid3 = fork();
            if (pid3 < 0) _exit(20);
            if (pid3 == 0) {
                /* great-grandchild: just exit */
                _exit(0);
            }
            int st3;
            waitpid(pid3, &st3, 0);
            if (!WIFEXITED(st3) || WEXITSTATUS(st3) != 0) _exit(21);
            _exit(0);
        }
        int st2;
        waitpid(pid2, &st2, 0);
        if (!WIFEXITED(st2) || WEXITSTATUS(st2) != 0) _exit(11);
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK("cow_stress: nested fork depth 3",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* -----------------------------------------------------------------------
 * 5. Fork+pipe+exit: child writes to pipe, parent reads, child exits.
 *    Stresses pipe fd cleanup during process exit + COW page table free.
 * ---------------------------------------------------------------------- */
static void test_fork_pipe_exit(void)
{
    int ok = 1;
    for (int i = 0; i < 10; i++) {
        int pfd[2];
        if (pipe(pfd) < 0) { ok = 0; break; }
        pid_t pid = fork();
        if (pid < 0) { close(pfd[0]); close(pfd[1]); ok = 0; break; }
        if (pid == 0) {
            close(pfd[0]);
            char msg[] = "pipe_ok";
            write(pfd[1], msg, 7);
            close(pfd[1]);
            _exit(0);
        }
        close(pfd[1]);
        char buf[16] = {0};
        read(pfd[0], buf, sizeof(buf));
        close(pfd[0]);
        int status;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
            memcmp(buf, "pipe_ok", 7) != 0) {
            ok = 0;
            break;
        }
    }
    CHECK("cow_stress: 10 fork+pipe+exit cycles", ok);
}

/* -----------------------------------------------------------------------
 * 6. Execve on non-ELF file: verify execve fails with proper error
 *    Stresses the exec error path + VFS fallback + process cleanup.
 * ---------------------------------------------------------------------- */
static void test_exec_non_elf(void)
{
    const char *path = "/mnt/stress_not_elf";
    unlink(path);
    int fd = open(path, O_CREAT | O_WRONLY, 0755);
    if (fd < 0) {
        CHECK("cow_stress: exec non-ELF returns error", 0);
        return;
    }
    write(fd, "NOT AN ELF FILE\n", 16);
    close(fd);

    pid_t pid = fork();
    if (pid < 0) { CHECK("cow_stress: exec non-ELF returns error", 0); return; }
    if (pid == 0) {
        char *argv[] = {"not_elf", NULL};
        char *envp[] = {NULL};
        int r = execve(path, argv, envp);
        /* execve must return -1 (it failed) */
        if (r == -1)
            _exit(0);
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    unlink(path);
    CHECK("cow_stress: exec non-ELF returns error",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* -----------------------------------------------------------------------
 * 7. COW write isolation: parent and child write to same stack variable.
 *    Verifies COW triggers correctly and pages are isolated.
 * ---------------------------------------------------------------------- */
static void test_cow_write_isolation(void)
{
    volatile int shared_var = 100;
    pid_t pid = fork();
    if (pid < 0) { CHECK("cow_stress: COW write isolation", 0); return; }
    if (pid == 0) {
        shared_var = 999;
        /* Verify our write stuck */
        if (shared_var != 999) _exit(1);
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    /* Parent's copy must still be 100 */
    CHECK("cow_stress: COW write isolation",
          WIFEXITED(status) && WEXITSTATUS(status) == 0 && shared_var == 100);
}

int main(void)
{
    printf("=== cowstresstest: COW + page table stress ===\n");

    test_rapid_fork_exit();
    test_fork_exec_exit();
    test_fork_ext2_io();
    test_nested_fork();
    test_fork_pipe_exit();
    test_exec_non_elf();
    test_cow_write_isolation();

    printf("=== cowstresstest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
