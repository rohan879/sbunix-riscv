/*
 * cowtest -- Copy-on-write fork verification tests.
 *
 * Verifies that fork() uses COW semantics: parent and child share
 * physical pages until one writes, at which point only the written
 * page is copied. Tests data isolation across globals, heap, stack,
 * mmap regions, and multi-level fork hierarchies.
 *
 * Tests:
 *  1. cow_parent_child_isolation -- child write does not affect parent
 *  2. cow_large_allocation       -- 64-page COW across heap
 *  3. cow_multilevel_fork        -- parent->child->grandchild isolation
 *  4. cow_fork_exec              -- fork+exec frees COW pages (no OOM)
 *  5. cow_refcount               -- 3 children sharing, each writes
 *  6. cow_text_readonly          -- text pages remain readable after fork
 *  7. cow_stack_isolation        -- child stack writes don't affect parent
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* Global data used by COW tests */
static int g_cow_global_int = 0xBEEF;
static char g_cow_global_buf[4096];

/* A known function address for the text-page test */
static int cow_text_probe(void)
{
    return 12345;
}

/* ===================================================================
 * 1. cow_parent_child_isolation
 *
 * Set a global variable and a heap buffer, fork, have the child write
 * different values to both, then verify the parent's copies are
 * unchanged. This is the fundamental COW correctness test.
 * =================================================================== */

static void test_cow_parent_child_isolation(void)
{
    /* Set up known values */
    g_cow_global_int = 100;
    memset(g_cow_global_buf, 'P', sizeof(g_cow_global_buf));

    char *heap = (char *)malloc(4096);
    if (!heap) {
        CHECK("cow_parent_child_isolation: malloc failed", 0);
        return;
    }
    memset(heap, 'H', 4096);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: modify everything */
        g_cow_global_int = 200;
        memset(g_cow_global_buf, 'C', sizeof(g_cow_global_buf));
        memset(heap, 'X', 4096);

        /* Verify child sees its own writes */
        if (g_cow_global_int != 200) _exit(1);
        if (g_cow_global_buf[0] != 'C') _exit(2);
        if (g_cow_global_buf[4095] != 'C') _exit(3);
        if (heap[0] != 'X') _exit(4);
        if (heap[4095] != 'X') _exit(5);
        _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK("cow_parent_child_isolation: child exited cleanly",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Verify parent's data is unchanged */
    CHECK("cow_parent_child_isolation: global int unchanged",
          g_cow_global_int == 100);

    int buf_ok = 1;
    int i;
    for (i = 0; i < (int)sizeof(g_cow_global_buf); i++) {
        if (g_cow_global_buf[i] != 'P') { buf_ok = 0; break; }
    }
    CHECK("cow_parent_child_isolation: global buffer unchanged", buf_ok);

    int heap_ok = 1;
    for (i = 0; i < 4096; i++) {
        if (heap[i] != 'H') { heap_ok = 0; break; }
    }
    CHECK("cow_parent_child_isolation: heap buffer unchanged", heap_ok);

    free(heap);
}

/* ===================================================================
 * 2. cow_large_allocation
 *
 * Allocate 64 pages (256 KB) via mmap, fill with a page-indexed
 * pattern, fork, child modifies every byte on every page, parent
 * verifies all 64 pages remain unmodified.
 * =================================================================== */

static void test_cow_large_allocation(void)
{
    const int NPAGES = 64;
    const size_t len = (size_t)NPAGES * 4096;

    unsigned char *p = (unsigned char *)mmap((void *)0, len,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK("cow_large_allocation: mmap != MAP_FAILED",
          p != (unsigned char *)MAP_FAILED);
    if (p == (unsigned char *)MAP_FAILED) return;

    /* Fill each page with its page number */
    int pg;
    for (pg = 0; pg < NPAGES; pg++)
        memset(p + pg * 4096, (unsigned char)pg, 4096);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: overwrite every page with 0xFF */
        for (pg = 0; pg < NPAGES; pg++)
            memset(p + pg * 4096, 0xFF, 4096);

        /* Verify child sees 0xFF everywhere */
        int ok = 1;
        size_t i;
        for (i = 0; i < len; i++) {
            if (p[i] != 0xFF) { ok = 0; break; }
        }
        _exit(ok ? 0 : 1);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK("cow_large_allocation: child modified all 64 pages",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Verify parent's data is unchanged */
    int parent_ok = 1;
    for (pg = 0; pg < NPAGES; pg++) {
        unsigned char expected = (unsigned char)pg;
        int j;
        for (j = 0; j < 4096; j++) {
            if (p[pg * 4096 + j] != expected) {
                parent_ok = 0;
                break;
            }
        }
        if (!parent_ok) break;
    }
    CHECK("cow_large_allocation: parent all 64 pages intact", parent_ok);

    munmap(p, len);
}

/* ===================================================================
 * 3. cow_multilevel_fork
 *
 * Parent forks child, child forks grandchild. All three share the
 * same initial data. Each writes a different value. When the dust
 * settles, each process must see only its own writes.
 *
 * Communication: child and grandchild encode their verification
 * results in their exit codes.
 * =================================================================== */

static int g_multi_val = 1000;

static void test_cow_multilevel_fork(void)
{
    g_multi_val = 1000;

    char *shared_page = (char *)mmap((void *)0, 4096,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (shared_page == MAP_FAILED) {
        CHECK("cow_multilevel_fork: mmap failed", 0);
        return;
    }
    memset(shared_page, 'A', 4096);

    pid_t child = fork();
    if (child == 0) {
        /* ---- CHILD ---- */
        /* Modify our copy */
        g_multi_val = 2000;
        memset(shared_page, 'B', 4096);

        pid_t grandchild = fork();
        if (grandchild == 0) {
            /* ---- GRANDCHILD ---- */
            /* Verify we inherited child's values */
            if (g_multi_val != 2000) _exit(10);
            if (shared_page[0] != 'B') _exit(11);

            /* Now modify our own copy */
            g_multi_val = 3000;
            memset(shared_page, 'C', 4096);

            /* Verify grandchild sees its own writes */
            if (g_multi_val != 3000) _exit(12);
            if (shared_page[0] != 'C') _exit(13);
            if (shared_page[4095] != 'C') _exit(14);
            _exit(0);
        }

        int gc_status = 0;
        waitpid(grandchild, &gc_status, 0);
        if (!WIFEXITED(gc_status) || WEXITSTATUS(gc_status) != 0)
            _exit(20 + WEXITSTATUS(gc_status));

        /* After grandchild exits, child's data must still be its own */
        if (g_multi_val != 2000) _exit(30);
        if (shared_page[0] != 'B') _exit(31);
        if (shared_page[4095] != 'B') _exit(32);
        _exit(0);
    }

    int status = 0;
    waitpid(child, &status, 0);
    CHECK("cow_multilevel_fork: child+grandchild both correct",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Parent's data must be unchanged */
    CHECK("cow_multilevel_fork: parent global unchanged",
          g_multi_val == 1000);

    int pg_ok = 1;
    int i;
    for (i = 0; i < 4096; i++) {
        if (shared_page[i] != 'A') { pg_ok = 0; break; }
    }
    CHECK("cow_multilevel_fork: parent mmap page unchanged", pg_ok);

    munmap(shared_page, 4096);
}

/* ===================================================================
 * 4. cow_fork_exec
 *
 * Fork, then the child execs /bin/true. The exec replaces the child's
 * address space, so all COW-shared pages must be properly freed (their
 * refcount decremented). Repeat 20 times. OOM = refcount leak.
 * =================================================================== */

static void test_cow_fork_exec(void)
{
    const int NITER = 20;

    /* Allocate a sizable region so each fork shares significant memory */
    const size_t alloc_sz = 32 * 4096;  /* 128 KB */
    unsigned char *region = (unsigned char *)mmap((void *)0, alloc_sz,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == (unsigned char *)MAP_FAILED) {
        CHECK("cow_fork_exec: mmap failed", 0);
        return;
    }
    memset(region, 0x42, alloc_sz);

    int all_ok = 1;
    int i;
    for (i = 0; i < NITER; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            all_ok = 0;
            break;
        }
        if (pid == 0) {
            /*
             * Child: exec /bin/true. This discards the entire COW
             * address space. If the kernel doesn't decrement refcounts
             * on shared pages, physical memory leaks.
             */
            char *argv[] = { "true", (char *)0 };
            char *envp[] = { (char *)0 };
            execve("/bin/true", argv, envp);
            _exit(127);
        }

        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            all_ok = 0;
            break;
        }
    }

    CHECK("cow_fork_exec: 20 fork+exec cycles without OOM",
          all_ok && i == NITER);

    /* Verify parent's region is still intact */
    int region_ok = 1;
    size_t j;
    for (j = 0; j < alloc_sz; j++) {
        if (region[j] != 0x42) { region_ok = 0; break; }
    }
    CHECK("cow_fork_exec: parent region unchanged after exec cycles",
          region_ok);

    munmap(region, alloc_sz);
}

/* ===================================================================
 * 5. cow_refcount
 *
 * Fork 3 children that all share the same data pages. Each child
 * writes a unique value to the shared pages, then exits. The parent
 * verifies its data is unchanged. This stresses COW reference counting:
 * a page shared by 4 processes (parent + 3 children) must have its
 * refcount correctly tracked so that the last user can safely modify
 * it without copying.
 * =================================================================== */

static void test_cow_refcount(void)
{
    const int NCHILDREN = 3;
    const size_t len = 8 * 4096;  /* 32 KB shared region */

    unsigned char *region = (unsigned char *)mmap((void *)0, len,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == (unsigned char *)MAP_FAILED) {
        CHECK("cow_refcount: mmap failed", 0);
        return;
    }
    memset(region, 0xAA, len);

    pid_t pids[3];
    int i;

    /* Fork all 3 children before any of them write */
    for (i = 0; i < NCHILDREN; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            CHECK("cow_refcount: fork failed", 0);
            munmap(region, len);
            return;
        }
        if (pids[i] == 0) {
            /* Child i: write a unique pattern */
            unsigned char pattern = (unsigned char)(0x10 + i);
            memset(region, pattern, len);

            /* Verify child's own writes */
            int ok = 1;
            size_t j;
            for (j = 0; j < len; j++) {
                if (region[j] != pattern) { ok = 0; break; }
            }
            _exit(ok ? 0 : 1);
        }
    }

    /* Parent: wait for all children */
    int all_ok = 1;
    for (i = 0; i < NCHILDREN; i++) {
        int status = 0;
        waitpid(pids[i], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            all_ok = 0;
    }
    CHECK("cow_refcount: all 3 children wrote independently",
          all_ok);

    /* Verify parent's data is unchanged */
    int parent_ok = 1;
    size_t j;
    for (j = 0; j < len; j++) {
        if (region[j] != 0xAA) { parent_ok = 0; break; }
    }
    CHECK("cow_refcount: parent region unchanged after 3 children",
          parent_ok);

    munmap(region, len);
}

/* ===================================================================
 * 6. cow_text_readonly
 *
 * After fork, the child should be able to call the same functions as
 * the parent (text pages are shared read-only). Verify that a function
 * pointer from before the fork still works in the child, returns the
 * correct value, and the function's address is the same.
 * =================================================================== */

static void test_cow_text_readonly(void)
{
    /* Capture function pointer before fork */
    int (*fn_ptr)(void) = cow_text_probe;
    int parent_result = fn_ptr();

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: call the same function via the saved pointer */
        int child_result = fn_ptr();
        if (child_result != 12345) _exit(1);

        /* Call directly too */
        int direct = cow_text_probe();
        if (direct != 12345) _exit(2);

        /* Verify function address is the same */
        int (*child_fn_ptr)(void) = cow_text_probe;
        if (child_fn_ptr != fn_ptr) _exit(3);

        _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK("cow_text_readonly: child calls function via shared text page",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Parent can still call the function */
    CHECK("cow_text_readonly: parent function still works",
          cow_text_probe() == 12345);

    CHECK("cow_text_readonly: parent result from before fork is correct",
          parent_result == 12345);
}

/* ===================================================================
 * 7. cow_stack_isolation
 *
 * Place a known pattern on the stack, fork, have the child overwrite
 * its stack copy with different data, then verify the parent's stack
 * data is unchanged. The child also pushes deeper into the stack to
 * ensure stack growth in the child does not corrupt the parent.
 * =================================================================== */

/* Helper: deeply recursive function to push child's stack */
static int stack_pusher(int depth, volatile char *canary)
{
    char local_buf[128];
    memset(local_buf, (char)depth, sizeof(local_buf));
    if (depth <= 0) {
        /* Verify the canary passed from the top is still accessible */
        return (*canary == 'Z') ? 0 : 1;
    }
    return stack_pusher(depth - 1, canary);
}

static void test_cow_stack_isolation(void)
{
    /* Large stack buffer with known pattern */
    volatile char stack_buf[512];
    int i;
    for (i = 0; i < 512; i++)
        stack_buf[i] = (char)(i % 127);

    /* A separate stack variable */
    volatile int stack_int = 0xABCD;

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: verify inherited stack data */
        int ok = 1;
        for (i = 0; i < 512; i++) {
            if (stack_buf[i] != (char)(i % 127)) { ok = 0; break; }
        }
        if (!ok) _exit(1);
        if (stack_int != 0xABCD) _exit(2);

        /* Now overwrite the stack buffer (triggers COW on stack page) */
        for (i = 0; i < 512; i++)
            stack_buf[i] = 'Z';
        stack_int = 0xDEAD;

        /* Verify child's writes stuck */
        for (i = 0; i < 512; i++) {
            if (stack_buf[i] != 'Z') _exit(3);
        }
        if (stack_int != 0xDEAD) _exit(4);

        /* Push deep into stack -- should not corrupt parent */
        volatile char canary = 'Z';
        int r = stack_pusher(50, &canary);
        if (r != 0) _exit(5);

        _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK("cow_stack_isolation: child stack ops succeeded",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Verify parent's stack data is unchanged */
    int parent_buf_ok = 1;
    for (i = 0; i < 512; i++) {
        if (stack_buf[i] != (char)(i % 127)) {
            parent_buf_ok = 0;
            break;
        }
    }
    CHECK("cow_stack_isolation: parent stack buffer unchanged",
          parent_buf_ok);

    CHECK("cow_stack_isolation: parent stack int unchanged",
          stack_int == 0xABCD);
}

/* =================================================================== */

int main(void)
{
    printf("=== cowtest: copy-on-write fork ===\n");

    test_cow_parent_child_isolation();
    test_cow_large_allocation();
    test_cow_multilevel_fork();
    test_cow_fork_exec();
    test_cow_refcount();
    test_cow_text_readonly();
    test_cow_stack_isolation();

    printf("=== cowtest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
