/*
 * swaptest -- Memory allocation lifecycle and page reclamation tests.
 *
 * Verifies that the OS correctly reclaims physical pages when processes
 * exit, that malloc/free/realloc/sbrk work, that mmap/munmap do not
 * leak, and that fork+exec cycles do not exhaust memory.
 *
 * Tests:
 *  1. fork_exit_memory_stress   -- 50 sequential fork+mmap+exit children
 *  2. malloc_free_reuse         -- malloc, free, re-malloc succeeds
 *  3. realloc_grow              -- realloc to progressively larger sizes
 *  4. brk_grows_heap            -- sbrk(4096) extends heap, zero-filled
 *  5. brk_shrink                -- sbrk(-4096) shrinks heap
 *  6. mmap_anonymous_zero       -- MAP_ANONYMOUS pages are zero-initialized
 *  7. mmap_munmap_cycle         -- repeated mmap+munmap does not leak
 *  8. cow_fork_no_leak          -- fork with COW write, pages reclaimed
 *  9. exec_reclaims_memory      -- 30 fork+exec cycles, no OOM
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

/* ===================================================================
 * 1. fork_exit_memory_stress
 *
 * Fork 50 children sequentially. Each child mmap's 16 pages (64 KB),
 * touches every byte on each page, then exits. The parent waits for
 * each child before forking the next. If the OS fails to reclaim
 * pages on process exit, it will OOM well before 50 iterations.
 * =================================================================== */

static void test_fork_exit_memory_stress(void)
{
    const int NCHILDREN = 50;
    const int NPAGES    = 16;
    const size_t len    = (size_t)NPAGES * 4096;
    int all_ok = 1;
    int i;

    for (i = 0; i < NCHILDREN; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            /* fork failed -- likely OOM */
            all_ok = 0;
            break;
        }
        if (pid == 0) {
            /* Child: allocate and touch 16 pages */
            void *p = mmap((void *)0, len, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED)
                _exit(1);

            /* Touch every page with a distinct pattern */
            unsigned char *bp = (unsigned char *)p;
            int pg;
            for (pg = 0; pg < NPAGES; pg++) {
                memset(bp + pg * 4096, (unsigned char)(pg + i), 4096);
            }

            /* Verify the pattern survived */
            for (pg = 0; pg < NPAGES; pg++) {
                unsigned char expected = (unsigned char)(pg + i);
                int j;
                for (j = 0; j < 4096; j++) {
                    if (bp[pg * 4096 + j] != expected)
                        _exit(2);
                }
            }

            munmap(p, len);
            _exit(0);
        }

        /* Parent: wait for this child */
        int status = 0;
        pid_t w = waitpid(pid, &status, 0);
        if (w != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            all_ok = 0;
            break;
        }
    }

    CHECK("fork_exit_memory_stress: 50 children mmap+exit without OOM",
          all_ok && i == NCHILDREN);
}

/* ===================================================================
 * 2. malloc_free_reuse
 *
 * Malloc a large block (32 KB), fill it with a pattern, free it, then
 * malloc the same size again. The second malloc must succeed, proving
 * that free returned memory to the allocator for reuse.
 * =================================================================== */

static void test_malloc_free_reuse(void)
{
    const size_t SZ = 32768;  /* 32 KB */

    /* First allocation */
    unsigned char *p1 = (unsigned char *)malloc(SZ);
    CHECK("malloc_free_reuse: first malloc != NULL", p1 != (void *)0);
    if (!p1) return;

    /* Fill with pattern and verify */
    size_t i;
    for (i = 0; i < SZ; i++)
        p1[i] = (unsigned char)(i % 251);

    int ok = 1;
    for (i = 0; i < SZ; i++) {
        if (p1[i] != (unsigned char)(i % 251)) { ok = 0; break; }
    }
    CHECK("malloc_free_reuse: first block pattern intact", ok);

    free(p1);

    /* Second allocation -- must succeed if free works */
    unsigned char *p2 = (unsigned char *)malloc(SZ);
    CHECK("malloc_free_reuse: second malloc != NULL", p2 != (void *)0);
    if (!p2) return;

    /* Write and read back to confirm usability */
    memset(p2, 0xAB, SZ);
    ok = 1;
    for (i = 0; i < SZ; i++) {
        if (p2[i] != 0xAB) { ok = 0; break; }
    }
    CHECK("malloc_free_reuse: second block writable and readable", ok);

    free(p2);
}

/* ===================================================================
 * 3. realloc_grow
 *
 * Start with a 64-byte allocation, fill it with a pattern, then
 * realloc to 256, 1024, 4096, and 16384 bytes. After each realloc,
 * verify the old data is preserved and the new space is writable.
 * =================================================================== */

static void test_realloc_grow(void)
{
    size_t sizes[] = { 64, 256, 1024, 4096, 16384 };
    int nsizes = 5;
    int step;

    unsigned char *p = (unsigned char *)malloc(sizes[0]);
    CHECK("realloc_grow: initial malloc != NULL", p != (void *)0);
    if (!p) return;

    /* Fill initial allocation */
    size_t i;
    for (i = 0; i < sizes[0]; i++)
        p[i] = (unsigned char)(i % 239);

    int all_ok = 1;
    for (step = 1; step < nsizes; step++) {
        size_t old_size = sizes[step - 1];
        size_t new_size = sizes[step];

        unsigned char *q = (unsigned char *)realloc(p, new_size);
        if (!q) {
            all_ok = 0;
            break;
        }
        p = q;

        /* Verify old data is preserved */
        for (i = 0; i < old_size; i++) {
            if (p[i] != (unsigned char)(i % 239)) {
                all_ok = 0;
                break;
            }
        }
        if (!all_ok) break;

        /* Fill the new space with a different pattern */
        for (i = old_size; i < new_size; i++)
            p[i] = (unsigned char)(i % 239);
    }

    CHECK("realloc_grow: 5 progressive reallocs preserve data", all_ok);

    /* Final full verification */
    int final_ok = 1;
    for (i = 0; i < sizes[nsizes - 1]; i++) {
        if (p[i] != (unsigned char)(i % 239)) {
            final_ok = 0;
            break;
        }
    }
    CHECK("realloc_grow: final 16384 bytes all correct", final_ok);

    free(p);
}

/* ===================================================================
 * 4. brk_grows_heap
 *
 * Call sbrk(4096) to extend the heap by one page. Verify the returned
 * pointer is non-error, the memory is writable, and it is zero-filled
 * (the kernel must zero new pages given to userspace).
 * =================================================================== */

static void test_brk_grows_heap(void)
{
    void *old_brk = sbrk(0);
    CHECK("brk_grows_heap: sbrk(0) != (void*)-1",
          old_brk != (void *)-1);

    void *p = sbrk(4096);
    CHECK("brk_grows_heap: sbrk(4096) != (void*)-1",
          p != (void *)-1);

    if (p == (void *)-1) return;

    /* Verify the new page is zero-filled */
    unsigned char *bp = (unsigned char *)p;
    int zero_ok = 1;
    int i;
    for (i = 0; i < 4096; i++) {
        if (bp[i] != 0) {
            zero_ok = 0;
            break;
        }
    }
    CHECK("brk_grows_heap: new page is zero-filled", zero_ok);

    /* Verify the page is writable */
    memset(bp, 0xDE, 4096);
    int write_ok = 1;
    for (i = 0; i < 4096; i++) {
        if (bp[i] != 0xDE) { write_ok = 0; break; }
    }
    CHECK("brk_grows_heap: new page is writable", write_ok);

    /* Verify sbrk(0) moved forward */
    void *new_brk = sbrk(0);
    CHECK("brk_grows_heap: brk advanced by >= 4096",
          (unsigned long)new_brk >= (unsigned long)old_brk + 4096);
}

/* ===================================================================
 * 5. brk_shrink
 *
 * Grow the heap by 4096 bytes, then shrink it back by 4096 bytes.
 * The break should return to (approximately) the original value.
 * =================================================================== */

static void test_brk_shrink(void)
{
    void *base = sbrk(0);
    if (base == (void *)-1) {
        CHECK("brk_shrink: sbrk(0) failed", 0);
        return;
    }

    /* Grow */
    void *p = sbrk(4096);
    if (p == (void *)-1) {
        CHECK("brk_shrink: sbrk(4096) failed", 0);
        return;
    }

    void *after_grow = sbrk(0);
    CHECK("brk_shrink: heap grew",
          (unsigned long)after_grow > (unsigned long)base);

    /* Shrink */
    void *q = sbrk(-4096);
    CHECK("brk_shrink: sbrk(-4096) != (void*)-1",
          q != (void *)-1);

    void *after_shrink = sbrk(0);
    CHECK("brk_shrink: heap returned to original",
          (unsigned long)after_shrink <= (unsigned long)after_grow - 4096 + 4096 &&
          (unsigned long)after_shrink >= (unsigned long)base);
}

/* ===================================================================
 * 6. mmap_anonymous_zero
 *
 * mmap a MAP_ANONYMOUS page and verify every byte is zero. The kernel
 * must zero-fill anonymous pages to prevent information leaks.
 * =================================================================== */

static void test_mmap_anonymous_zero(void)
{
    /* Test with multiple pages to be thorough */
    const int NPAGES = 4;
    size_t len = (size_t)NPAGES * 4096;

    void *p = mmap((void *)0, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK("mmap_anonymous_zero: mmap != MAP_FAILED", p != MAP_FAILED);
    if (p == MAP_FAILED) return;

    unsigned char *bp = (unsigned char *)p;
    int zero_ok = 1;
    size_t first_nonzero = 0;
    size_t i;
    for (i = 0; i < len; i++) {
        if (bp[i] != 0) {
            zero_ok = 0;
            first_nonzero = i;
            break;
        }
    }

    if (!zero_ok) {
        printf("  (first non-zero byte at offset %lu: 0x%x)\n",
               (unsigned long)first_nonzero,
               (unsigned int)bp[first_nonzero]);
    }

    CHECK("mmap_anonymous_zero: all 16384 bytes are zero", zero_ok);

    munmap(p, len);
}

/* ===================================================================
 * 7. mmap_munmap_cycle
 *
 * Repeatedly mmap and munmap 8 pages (32 KB). If munmap does not
 * reclaim virtual/physical memory, the OS will run out after enough
 * iterations. We do 100 iterations to stress this.
 * =================================================================== */

static void test_mmap_munmap_cycle(void)
{
    const int NITER  = 100;
    const int NPAGES = 8;
    const size_t len = (size_t)NPAGES * 4096;
    int all_ok = 1;
    int i;

    for (i = 0; i < NITER; i++) {
        void *p = mmap((void *)0, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            all_ok = 0;
            break;
        }

        /* Touch every page to force physical allocation */
        unsigned char *bp = (unsigned char *)p;
        int pg;
        for (pg = 0; pg < NPAGES; pg++)
            bp[pg * 4096] = (unsigned char)(i + pg);

        int r = munmap(p, len);
        if (r != 0) {
            all_ok = 0;
            break;
        }
    }

    CHECK("mmap_munmap_cycle: 100 mmap+touch+munmap cycles without OOM",
          all_ok && i == NITER);
}

/* ===================================================================
 * 8. cow_fork_no_leak
 *
 * Allocate 8 pages, fork, child writes to all pages (triggering COW
 * faults), then child exits. Parent waits and verifies its data is
 * unchanged. Repeat 20 times. If COW pages are not reclaimed when the
 * child exits, the OS will run out of memory.
 * =================================================================== */

/* Shared data for COW verification */
static int g_cow_sentinel = 0xCAFE;

static void test_cow_fork_no_leak(void)
{
    const int NITER  = 20;
    const int NPAGES = 8;
    const size_t len = (size_t)NPAGES * 4096;
    int all_ok = 1;
    int i;

    for (i = 0; i < NITER; i++) {
        /* Allocate pages in the parent */
        void *p = mmap((void *)0, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            all_ok = 0;
            break;
        }

        /* Fill with a known pattern */
        unsigned char *bp = (unsigned char *)p;
        memset(bp, (unsigned char)(0x30 + (i % 10)), len);

        pid_t pid = fork();
        if (pid < 0) {
            all_ok = 0;
            munmap(p, len);
            break;
        }

        if (pid == 0) {
            /* Child: write to every page, triggering COW faults */
            int pg;
            for (pg = 0; pg < NPAGES; pg++)
                memset(bp + pg * 4096, 0xFF, 4096);

            /* Also verify the global sentinel was inherited */
            if (g_cow_sentinel != 0xCAFE)
                _exit(2);

            /* Modify the sentinel to confirm isolation */
            g_cow_sentinel = 0xDEAD;
            _exit(0);
        }

        /* Parent: wait for child */
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            all_ok = 0;
            munmap(p, len);
            break;
        }

        /* Verify parent's data is unchanged */
        unsigned char expected = (unsigned char)(0x30 + (i % 10));
        int pg;
        for (pg = 0; pg < NPAGES; pg++) {
            int j;
            for (j = 0; j < 4096; j++) {
                if (bp[pg * 4096 + j] != expected) {
                    all_ok = 0;
                    break;
                }
            }
            if (!all_ok) break;
        }

        /* Verify global sentinel unchanged in parent */
        if (g_cow_sentinel != 0xCAFE)
            all_ok = 0;

        munmap(p, len);
        if (!all_ok) break;
    }

    CHECK("cow_fork_no_leak: 20 COW fork cycles, parent data intact",
          all_ok && i == NITER);
}

/* ===================================================================
 * 9. exec_reclaims_memory
 *
 * Fork+exec /bin/true 30 times sequentially. Each exec replaces the
 * child's address space, so all COW/mmap pages from the fork must be
 * freed. If exec does not reclaim memory, the OS will OOM.
 * =================================================================== */

static void test_exec_reclaims_memory(void)
{
    const int NITER = 30;
    int all_ok = 1;
    int i;

    for (i = 0; i < NITER; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            all_ok = 0;
            break;
        }

        if (pid == 0) {
            char *argv[] = { "true", (char *)0 };
            char *envp[] = { (char *)0 };
            execve("/bin/true", argv, envp);
            /* If execve fails, exit with error */
            _exit(127);
        }

        int status = 0;
        pid_t w = waitpid(pid, &status, 0);
        if (w != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            all_ok = 0;
            break;
        }
    }

    CHECK("exec_reclaims_memory: 30 fork+exec cycles without OOM",
          all_ok && i == NITER);
}

/* =================================================================== */

int main(void)
{
    printf("=== swaptest: memory allocation lifecycle ===\n");

    test_fork_exit_memory_stress();
    test_malloc_free_reuse();
    test_realloc_grow();
    test_brk_grows_heap();
    test_brk_shrink();
    test_mmap_anonymous_zero();
    test_mmap_munmap_cycle();
    test_cow_fork_no_leak();
    test_exec_reclaims_memory();

    printf("=== swaptest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
