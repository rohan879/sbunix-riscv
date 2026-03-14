/*
 * sectest.c -- Security boundary tests for SBUnix.
 *
 * Verifies that the kernel correctly rejects illegal operations:
 *   1. mmap MAP_FIXED at a kernel virtual address
 *   2. brk at an unreasonably high address
 *   3. execve of a non-existent file
 *   4. fork returns a valid pid (single fork, not a fork-bomb)
 *   5. fd exhaustion (open until EMFILE)
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else      { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * 1. mmap MAP_FIXED at kernel VA should fail
 * ---------------------------------------------------------------------- */

static void test_mmap_kernel_va(void) {
    /*
     * 0x80200000 is the kernel load address.  A user-mode mmap
     * with MAP_FIXED at that address must be rejected.
     */
    void *p = mmap((void *)0x80200000UL, 4096,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                   -1, 0);
    CHECK("sec: mmap MAP_FIXED at kernel VA fails", p == MAP_FAILED);
    if (p != MAP_FAILED)
        munmap(p, 4096);
}

/* -----------------------------------------------------------------------
 * 2. brk at very high address should fail
 * ---------------------------------------------------------------------- */

static void test_brk_high(void) {
    /*
     * Try to set the program break to an absurdly high address
     * (well above MAXVA).  The kernel must reject this.
     */
    int r = brk((void *)0xFFFFFFFF00000000UL);
    CHECK("sec: brk at very high address fails", r == -1);
}

/* -----------------------------------------------------------------------
 * 3. execve of non-existent file returns error
 * ---------------------------------------------------------------------- */

static void test_exec_nonexistent(void) {
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = { "nosuchbin", (void *)0 };
        char *envp[] = { (void *)0 };
        int r = execve("/no/such/binary", argv, envp);
        /* If we get here, execve failed as expected */
        _exit(r == -1 ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK("sec: execve nonexistent returns error",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* -----------------------------------------------------------------------
 * 4. fork returns valid pid (single fork, not a fork-bomb)
 * ---------------------------------------------------------------------- */

static void test_fork_valid(void) {
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: just exit immediately */
        _exit(0);
    }
    CHECK("sec: fork returns pid > 0", pid > 0);
    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK("sec: forked child exited cleanly",
              WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}

/* -----------------------------------------------------------------------
 * 5. fd exhaustion -- open files until we hit the limit
 * ---------------------------------------------------------------------- */

static void test_fd_exhaustion(void) {
    int fds[256];
    int count = 0;
    int hit_limit = 0;

    /* Open /dev/null (or any file that exists) repeatedly */
    while (count < 256) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) {
            /* We expect EMFILE when the fd table is full */
            hit_limit = 1;
            break;
        }
        fds[count++] = fd;
    }

    CHECK("sec: fd exhaustion reached limit", hit_limit);

    /* Close all the fds we opened */
    for (int i = 0; i < count; i++)
        close(fds[i]);
}

/* -----------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void) {
    printf("=== sectest: security boundary tests ===\n");

    test_mmap_kernel_va();
    test_brk_high();
    test_exec_nonexistent();
    test_fork_valid();
    test_fd_exhaustion();

    printf("=== sectest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
