/*
 * swaptest -- verify swap, paging, mmap, COW fork, and stack auto-grow.
 *
 * Tests:
 *  1. mmap_anon_rw        -- basic anonymous mmap read/write
 *  2. mmap_multipage      -- 16-page mmap with pattern verification
 *  3. cow_fork_isolation  -- COW fork does not leak writes to parent
 *  4. cow_fork_both_run   -- both parent and child survive COW properly
 *  5. mprotect_read_only  -- mprotect(PROT_READ) causes SIGSEGV on write
 *  6. munmap_basic        -- munmap returns 0
 *  7. stack_auto_grow     -- deep recursion grows the stack automatically
 *  8. malloc_large        -- 32KB malloc with pattern fill/verify
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* SIGSEGV handler for mprotect test -- child exits with code 42 */
static void segv_exit(int sig) { (void)sig; _exit(42); }

/* COW fork isolation: global variable shared before fork */
static int g_cow_var = 100;

/* Stack auto-grow: recursive function with non-trivial stack usage */
static int recurse(int depth) {
    char pad[64];  /* force stack usage */
    pad[0] = (char)depth;
    if (depth <= 0) return pad[0];
    return recurse(depth - 1) + pad[0];
}

/* --------------------------------------------------------------------- */

static void test_mmap_anon_rw(void) {
    void *p = mmap((void *)0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK("mmap_anon_rw: mmap != MAP_FAILED", p != MAP_FAILED);
    if (p == MAP_FAILED) return;

    /* Write a pattern and read it back */
    memset(p, 0xA5, 4096);
    int ok = 1;
    for (int i = 0; i < 4096; i++) {
        if (((unsigned char *)p)[i] != 0xA5) { ok = 0; break; }
    }
    CHECK("mmap_anon_rw: pattern readback", ok);

    munmap(p, 4096);
}

/* --------------------------------------------------------------------- */

static void test_mmap_multipage(void) {
    size_t len = 16 * 4096;  /* 65536 bytes = 16 pages */
    void *p = mmap((void *)0, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK("mmap_multipage: mmap != MAP_FAILED", p != MAP_FAILED);
    if (p == MAP_FAILED) return;

    /* Fill with pattern: each byte = index % 253 */
    unsigned char *bp = (unsigned char *)p;
    for (size_t i = 0; i < len; i++)
        bp[i] = (unsigned char)(i % 253);

    /* Read back and verify */
    int ok = 1;
    for (size_t i = 0; i < len; i++) {
        if (bp[i] != (unsigned char)(i % 253)) { ok = 0; break; }
    }
    CHECK("mmap_multipage: all 65536 bytes match", ok);

    munmap(p, len);
}

/* --------------------------------------------------------------------- */

static void test_cow_fork_isolation(void) {
    g_cow_var = 100;

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: modify the global variable and exit */
        g_cow_var = 200;
        _exit(0);
    }

    /* Parent: wait for child, then verify our copy is unchanged */
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK("cow_fork_isolation: parent var == 100", g_cow_var == 100);
}

/* --------------------------------------------------------------------- */

static void test_cow_fork_both_run(void) {
    volatile int parent_work = 0;

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: do some work to exercise COW page copies */
        volatile int child_work = 0;
        for (int i = 0; i < 100; i++)
            child_work += i;
        /* Use child_work to prevent optimisation away */
        _exit(child_work > 0 ? 0 : 1);
    }

    /* Parent: do some work too */
    for (int i = 0; i < 100; i++)
        parent_work += i;

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK("cow_fork_both_run: child exited normally",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* --------------------------------------------------------------------- */

static void test_mprotect_read_only(void) {
    void *p = mmap((void *)0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        CHECK("mprotect_read_only: mmap failed", 0);
        return;
    }

    /* Write data while still RW */
    memset(p, 0xBB, 4096);

    /* Downgrade to read-only */
    mprotect(p, 4096, PROT_READ);

    /* Fork a child that tries to write -- should SIGSEGV */
    pid_t pid = fork();
    if (pid == 0) {
        /* Install SIGSEGV handler BEFORE the write attempt */
        struct sigaction sa;
        sa.sa_handler = segv_exit;
        sa.sa_mask = 0;
        sa.sa_flags = 0;
        sa.sa_restorer = 0;
        sigaction(SIGSEGV, &sa, 0);

        /* This write should fault */
        ((volatile char *)p)[0] = 0xFF;

        /* If we reach here, the mprotect did not work */
        _exit(1);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK("mprotect_read_only: write faulted (exit 42)",
          WIFEXITED(status) && WEXITSTATUS(status) == 42);

    /* Restore RW so we can munmap cleanly */
    mprotect(p, 4096, PROT_READ | PROT_WRITE);
    munmap(p, 4096);
}

/* --------------------------------------------------------------------- */

static void test_munmap_basic(void) {
    void *p = mmap((void *)0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        CHECK("munmap_basic: mmap failed", 0);
        return;
    }

    /* Write something so the page is definitely faulted in */
    ((volatile char *)p)[0] = 0xCC;

    int r = munmap(p, 4096);
    CHECK("munmap_basic: munmap returns 0", r == 0);
}

/* --------------------------------------------------------------------- */

static void test_stack_auto_grow(void) {
    int r = recurse(200);
    (void)r;  /* we only care that we survived */
    CHECK("stack_auto_grow: 200-deep recursion ok", 1);
}

/* --------------------------------------------------------------------- */

static void test_malloc_large(void) {
    size_t len = 32768;  /* 32KB */
    unsigned char *p = (unsigned char *)malloc(len);
    CHECK("malloc_large: ptr != NULL", p != (void *)0);
    if (!p) return;

    /* Fill with pattern: each byte = index % 251 */
    for (size_t i = 0; i < len; i++)
        p[i] = (unsigned char)(i % 251);

    /* Read back and verify */
    int ok = 1;
    for (size_t i = 0; i < len; i++) {
        if (p[i] != (unsigned char)(i % 251)) { ok = 0; break; }
    }
    CHECK("malloc_large: 32768 bytes pattern match", ok);

    free(p);
}

/* --------------------------------------------------------------------- */

int main(void) {
    printf("=== swaptest: swap and paging ===\n");

    test_mmap_anon_rw();
    test_mmap_multipage();
    test_cow_fork_isolation();
    test_cow_fork_both_run();
    test_mprotect_read_only();
    test_munmap_basic();
    test_stack_auto_grow();
    test_malloc_large();

    printf("=== swaptest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
