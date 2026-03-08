/*
 * mmaptest — verify mmap, munmap, and mprotect work correctly.
 *
 * Tests:
 *  1. mmap returns a valid pointer (not MAP_FAILED)
 *  2. mmap'd memory is writable (PROT_READ|PROT_WRITE)
 *  3. mmap'd memory preserves written data on read-back
 *  4. mprotect to PROT_READ succeeds (returns 0)
 *  5. Reading after mprotect(PROT_READ) still works
 *  6. Writing after mprotect(PROT_READ) triggers SIGSEGV
 *  7. munmap succeeds (returns 0)
 *  8. Multiple mmap allocations return distinct addresses
 *  9. Large mmap allocation works (multi-page)
 * 10. mmap with PROT_READ only — writing triggers SIGSEGV
 */

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>

static int g_pass = 0;
static int g_fail = 0;
static volatile int got_segv = 0;

#define CHECK(label, cond) do { \
    if (cond) { \
        printf("  PASS  %s\n", label); \
        g_pass++; \
    } else { \
        printf("  FAIL  %s\n", label); \
        g_fail++; \
    } \
} while (0)

static void segv_handler(int sig) {
    (void)sig;
    got_segv = 1;
    /* Cannot return to faulting instruction — just exit with known code */
    _exit(0);
}

int main(void) {
    printf("=== mmap/mprotect test suite ===\n");

    /* Test 1: basic mmap */
    void *p = mmap((void *)0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK("mmap != MAP_FAILED", p != MAP_FAILED);
    if (p == MAP_FAILED) {
        printf("\n=== mmaptest: %d passed, %d failed ===\n", g_pass, g_fail);
        return 1;
    }

    /* Test 2: writable */
    volatile char *cp = (volatile char *)p;
    cp[0] = 0xAB;
    CHECK("mmap writable", cp[0] == (char)0xAB);

    /* Test 3: fill and read back */
    memset(p, 0xCD, 4096);
    int ok = 1;
    for (int i = 0; i < 4096; i++) {
        if (((unsigned char *)p)[i] != 0xCD) { ok = 0; break; }
    }
    CHECK("mmap fill 4096 bytes", ok);

    /* Test 4: mprotect to PROT_READ */
    int r = mprotect(p, 4096, PROT_READ);
    CHECK("mprotect(PROT_READ) == 0", r == 0);

    /* Test 5: reading after mprotect still works */
    volatile char val = cp[0];
    CHECK("read after mprotect ok", val == (char)0xCD);

    /* Test 6: write after mprotect(PROT_READ) causes SIGSEGV
     * We fork a child to test this — the child writes and should get SIGSEGV. */
    {
        int child = fork();
        if (child == 0) {
            /* Child: set up SIGSEGV handler then write to read-only page */
            struct sigaction sa;
            sa.sa_handler = segv_handler;
            sa.sa_mask = 0;
            sa.sa_flags = 0;
            sa.sa_restorer = 0;
            sigaction(SIGSEGV, &sa, 0);

            got_segv = 0;
            cp[0] = 0xFF; /* should fault */

            /* If we get here, no fault occurred */
            _exit(1);
        } else {
            int status = 0;
            waitpid(child, &status, 0);
            /* Child exits 0 if SIGSEGV caught, 1 if no fault */
            int child_ok = (status >> 8) == 0;
            CHECK("write after mprotect(READ) faults", child_ok);
        }
    }

    /* Test 7: mprotect back to RW, then munmap */
    mprotect(p, 4096, PROT_READ | PROT_WRITE);
    r = munmap(p, 4096);
    CHECK("munmap == 0", r == 0);

    /* Test 8: multiple allocations are distinct */
    void *a = mmap(0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *b = mmap(0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK("two mmaps differ", a != MAP_FAILED && b != MAP_FAILED && a != b);
    if (a != MAP_FAILED) munmap(a, 4096);
    if (b != MAP_FAILED) munmap(b, 4096);

    /* Test 9: multi-page mmap (16KB) */
    void *big = mmap(0, 4 * 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK("multi-page mmap ok", big != MAP_FAILED);
    if (big != MAP_FAILED) {
        memset(big, 0x42, 4 * 4096);
        ok = 1;
        for (int i = 0; i < 4 * 4096; i++) {
            if (((unsigned char *)big)[i] != 0x42) { ok = 0; break; }
        }
        CHECK("multi-page fill ok", ok);
        munmap(big, 4 * 4096);
    }

    /* Test 10: mmap with PROT_READ only — write should fault */
    {
        void *ro = mmap(0, 4096, PROT_READ,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK("mmap PROT_READ ok", ro != MAP_FAILED);
        if (ro != MAP_FAILED) {
            int child2 = fork();
            if (child2 == 0) {
                struct sigaction sa;
                sa.sa_handler = segv_handler;
                sa.sa_mask = 0;
                sa.sa_flags = 0;
                sa.sa_restorer = 0;
                sigaction(SIGSEGV, &sa, 0);

                got_segv = 0;
                ((volatile char *)ro)[0] = 0xFF; /* should fault */
                _exit(1); /* no fault = fail */
            } else {
                int status = 0;
                waitpid(child2, &status, 0);
                int child_ok = (status >> 8) == 0;
                CHECK("write to PROT_READ mmap faults", child_ok);
            }
            munmap(ro, 4096);
        }
    }

    printf("\n=== mmaptest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
