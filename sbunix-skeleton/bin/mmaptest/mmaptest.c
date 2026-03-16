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
 * 11. mmap PROT_READ|PROT_EXEC — mapping succeeds
 * 12. Execute code from PROT_EXEC page (JIT: li a0,42; ret)
 * 13. W^X transition: mmap RW, write code, mprotect RX, execute
 * 14. Execute after munmap of PROT_EXEC page triggers fault
 * 15. Multi-function JIT: two functions on same PROT_EXEC page
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

    /*
     * ======== PROT_EXEC tests ========
     *
     * RISC-V RV64IMAC instructions used:
     *   li a0, N  =>  addi a0, zero, N  =>  0x00N00513  (for small N)
     *   ret       =>  jalr zero, ra, 0  =>  0x00008067
     *   add a0, a0, a1                  =>  0x00b50533
     *
     * After writing instructions we issue fence.i to synchronize the
     * instruction cache with the data writes (required by RISC-V).
     */

    /* Test 11: mmap with PROT_READ|PROT_EXEC succeeds */
    {
        void *xp = mmap(0, 4096, PROT_READ | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK("mmap PROT_READ|PROT_EXEC ok", xp != MAP_FAILED);
        if (xp != MAP_FAILED)
            munmap(xp, 4096);
    }

    /* Test 12: JIT — write code to RWX page, execute it
     *   function returns 42: addi a0, zero, 42; ret */
    {
        void *jit = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK("mmap RWX ok", jit != MAP_FAILED);
        if (jit != MAP_FAILED) {
            unsigned int *code = (unsigned int *)jit;
            code[0] = 0x02a00513;  /* addi a0, zero, 42 */
            code[1] = 0x00008067;  /* ret (jalr zero, ra, 0) */

            /* fence.i: flush icache so CPU sees the new instructions */
            __asm__ volatile ("fence.i" ::: "memory");

            typedef int (*fn_t)(void);
            fn_t fn = (fn_t)jit;
            int result = fn();
            CHECK("JIT exec returns 42", result == 42);
            munmap(jit, 4096);
        }
    }

    /* Test 13: W^X transition — mmap RW, write code, mprotect RX, execute
     * This is the pattern TCC and other JIT compilers use. */
    {
        void *wx = mmap(0, 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK("mmap RW for W^X ok", wx != MAP_FAILED);
        if (wx != MAP_FAILED) {
            /* Phase 1: write code while page is RW */
            unsigned int *code = (unsigned int *)wx;
            code[0] = 0x00700513;  /* addi a0, zero, 7 */
            code[1] = 0x00008067;  /* ret */

            /* Phase 2: transition to RX (remove write, add exec) */
            r = mprotect(wx, 4096, PROT_READ | PROT_EXEC);
            CHECK("mprotect RW->RX ok", r == 0);

            /* fence.i: required after writing instructions */
            __asm__ volatile ("fence.i" ::: "memory");

            /* Phase 3: execute */
            typedef int (*fn_t)(void);
            fn_t fn = (fn_t)wx;
            int result = fn();
            CHECK("W^X exec returns 7", result == 7);

            /* Phase 4: verify write is now blocked */
            {
                int child_wx = fork();
                if (child_wx == 0) {
                    struct sigaction sa;
                    sa.sa_handler = segv_handler;
                    sa.sa_mask = 0;
                    sa.sa_flags = 0;
                    sa.sa_restorer = 0;
                    sigaction(SIGSEGV, &sa, 0);
                    ((volatile char *)wx)[0] = 0xFF; /* should fault */
                    _exit(1);
                } else {
                    int status = 0;
                    waitpid(child_wx, &status, 0);
                    int child_ok = (status >> 8) == 0;
                    CHECK("write to RX page faults", child_ok);
                }
            }
            munmap(wx, 4096);
        }
    }

    /* Test 14: execute after munmap triggers fault */
    {
        void *gone = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (gone != MAP_FAILED) {
            unsigned int *code = (unsigned int *)gone;
            code[0] = 0x00100513;  /* addi a0, zero, 1 */
            code[1] = 0x00008067;  /* ret */
            __asm__ volatile ("fence.i" ::: "memory");

            /* Verify it works before unmap */
            typedef int (*fn_t)(void);
            fn_t fn = (fn_t)gone;
            int pre = fn();

            munmap(gone, 4096);

            /* Now call into unmapped memory — child should fault */
            int child_gone = fork();
            if (child_gone == 0) {
                struct sigaction sa;
                sa.sa_handler = segv_handler;
                sa.sa_mask = 0;
                sa.sa_flags = 0;
                sa.sa_restorer = 0;
                sigaction(SIGSEGV, &sa, 0);
                fn(); /* should fault — page is gone */
                _exit(1);
            } else {
                int status = 0;
                waitpid(child_gone, &status, 0);
                int child_ok = (status >> 8) == 0 && pre == 1;
                CHECK("exec after munmap faults", child_ok);
            }
        }
    }

    /* Test 15: two JIT functions on the same page, both callable */
    {
        void *multi = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK("mmap for multi-fn JIT ok", multi != MAP_FAILED);
        if (multi != MAP_FAILED) {
            unsigned int *code = (unsigned int *)multi;

            /* fn_a at offset 0: returns 10 */
            code[0] = 0x00a00513;  /* addi a0, zero, 10 */
            code[1] = 0x00008067;  /* ret */

            /* fn_b at offset 8: a0 = a0 + a1, ret (add two args) */
            code[2] = 0x00b50533;  /* add a0, a0, a1 */
            code[3] = 0x00008067;  /* ret */

            __asm__ volatile ("fence.i" ::: "memory");

            typedef int (*fn0_t)(void);
            typedef int (*fn2_t)(int, int);
            fn0_t fn_a = (fn0_t)&code[0];
            fn2_t fn_b = (fn2_t)&code[2];

            int ra = fn_a();
            int rb = fn_b(30, 12);
            CHECK("multi-fn JIT: fn_a returns 10", ra == 10);
            CHECK("multi-fn JIT: fn_b(30,12) returns 42", rb == 42);
            munmap(multi, 4096);
        }
    }

    printf("\n=== mmaptest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
