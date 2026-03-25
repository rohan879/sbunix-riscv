/*
 * lazytest -- lazy/demand page allocation tests
 *
 * Exercises sbrk, mmap (anonymous), munmap, malloc/free, and verifies
 * that pages are zero-filled and lazily allocated.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] lazy: %s\n", label); g_pass++; } \
    else      { printf("[FAIL] lazy: %s\n", label); g_fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * Helper: read an integer value from /proc/meminfo for a given key.
 * Returns the value in kB, or -1 on failure.
 * ----------------------------------------------------------------------- */
static long read_meminfo_kb(const char *key) {
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return -1;
    char buf[2048];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    char *p = strstr(buf, key);
    if (!p) return -1;
    p += strlen(key);
    /* skip ':' and spaces */
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    long val = 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    return val;
}

/* -----------------------------------------------------------------------
 * 1. sbrk_zero_returns_current: sbrk(0) returns current break
 * ----------------------------------------------------------------------- */
static void test_sbrk_zero_returns_current(void) {
    void *brk1 = sbrk(0);
    void *brk2 = sbrk(0);
    CHECK("sbrk_zero_returns_current",
          brk1 != (void *)-1 && brk2 != (void *)-1 && brk1 == brk2);
}

/* -----------------------------------------------------------------------
 * 2. sbrk_grow_writable: sbrk(4096), write to new page, read back
 * ----------------------------------------------------------------------- */
static void test_sbrk_grow_writable(void) {
    void *old = sbrk(0);
    void *p = sbrk(4096);
    CHECK("sbrk_grow_writable",
          p != (void *)-1 && p == old &&
          (((volatile char *)p)[0] = 0xAB, ((volatile char *)p)[0] == (char)0xAB));
}

/* -----------------------------------------------------------------------
 * 3. sbrk_pages_zero_filled: sbrk(4096), all bytes in new page are 0
 * ----------------------------------------------------------------------- */
static void test_sbrk_pages_zero_filled(void) {
    char *p = sbrk(4096);
    int ok = (p != (void *)-1);
    if (ok) {
        for (int i = 0; i < 4096; i++) {
            if (p[i] != 0) { ok = 0; break; }
        }
    }
    CHECK("sbrk_pages_zero_filled", ok);
}

/* -----------------------------------------------------------------------
 * 4. sbrk_large_alloc: sbrk(64*4096) succeeds without immediate OOM
 * ----------------------------------------------------------------------- */
static void test_sbrk_large_alloc(void) {
    void *p = sbrk(64 * 4096);
    CHECK("sbrk_large_alloc", p != (void *)-1);
}

/* -----------------------------------------------------------------------
 * 5. sbrk_touch_first_last: After large sbrk, touch first and last page
 * ----------------------------------------------------------------------- */
static void test_sbrk_touch_first_last(void) {
    char *p = sbrk(64 * 4096);
    if (p == (void *)-1) {
        CHECK("sbrk_touch_first_last", 0);
        return;
    }
    volatile char *vp = (volatile char *)p;
    vp[0] = 'A';
    vp[64 * 4096 - 1] = 'Z';
    CHECK("sbrk_touch_first_last", vp[0] == 'A' && vp[64 * 4096 - 1] == 'Z');
}

/* -----------------------------------------------------------------------
 * 6. sbrk_random_access: After large sbrk, touch pages out of order
 * ----------------------------------------------------------------------- */
static void test_sbrk_random_access(void) {
    char *p = sbrk(64 * 4096);
    if (p == (void *)-1) {
        CHECK("sbrk_random_access", 0);
        return;
    }
    volatile char *vp = (volatile char *)p;
    /* Touch pages in a scattered order: last, middle, first, 3/4, 1/4 */
    int offsets[] = { 63 * 4096, 32 * 4096, 0, 48 * 4096, 16 * 4096 };
    int ok = 1;
    for (int i = 0; i < 5; i++) {
        vp[offsets[i]] = (char)(i + 1);
    }
    for (int i = 0; i < 5; i++) {
        if (vp[offsets[i]] != (char)(i + 1)) { ok = 0; break; }
    }
    CHECK("sbrk_random_access", ok);
}

/* -----------------------------------------------------------------------
 * 7. mmap_anon_zero: mmap MAP_ANONYMOUS, all bytes are 0
 * ----------------------------------------------------------------------- */
static void test_mmap_anon_zero(void) {
    char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int ok = (p != MAP_FAILED);
    if (ok) {
        for (int i = 0; i < 4096; i++) {
            if (p[i] != 0) { ok = 0; break; }
        }
        munmap(p, 4096);
    }
    CHECK("mmap_anon_zero", ok);
}

/* -----------------------------------------------------------------------
 * 8. mmap_anon_lazy: mmap large region, check /proc/meminfo before/after
 *    touching -- memfree shouldn't drop much until pages are touched.
 * ----------------------------------------------------------------------- */
static void test_mmap_anon_lazy(void) {
    long free_before = read_meminfo_kb("MemFree");

    /* Map 256 pages (1 MB) but do NOT touch them yet */
    size_t sz = 256 * 4096;
    char *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED || free_before < 0) {
        CHECK("mmap_anon_lazy", p != MAP_FAILED);
        return;
    }

    long free_after_mmap = read_meminfo_kb("MemFree");

    /*
     * Lazy allocation: free memory should not have dropped by more than
     * a small amount (some kB for metadata) after mmap but before touching.
     * We allow a 128 kB margin for bookkeeping.
     */
    int lazy_ok = (free_before - free_after_mmap) < 128;

    /* Now touch every page */
    for (size_t i = 0; i < sz; i += 4096)
        p[i] = 1;

    long free_after_touch = read_meminfo_kb("MemFree");

    /*
     * After touching all 256 pages (1024 kB), free memory should have
     * dropped by at least 512 kB (allowing some margin for reclaim etc).
     */
    int touch_ok = (free_after_mmap - free_after_touch) > 512;

    munmap(p, sz);
    CHECK("mmap_anon_lazy", lazy_ok && touch_ok);
}

/* -----------------------------------------------------------------------
 * 9. mmap_munmap_reuse: mmap, munmap, mmap again -- works
 * ----------------------------------------------------------------------- */
static void test_mmap_munmap_reuse(void) {
    void *p1 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int ok = (p1 != MAP_FAILED);
    if (ok) {
        ((volatile char *)p1)[0] = 'X';
        int r = munmap(p1, 4096);
        ok = (r == 0);
    }
    if (ok) {
        void *p2 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ok = (p2 != MAP_FAILED);
        if (ok) {
            ((volatile char *)p2)[0] = 'Y';
            ok = (((volatile char *)p2)[0] == 'Y');
            munmap(p2, 4096);
        }
    }
    CHECK("mmap_munmap_reuse", ok);
}

/* -----------------------------------------------------------------------
 * 10. malloc_free_cycle: malloc/free 100 times -- no OOM
 * ----------------------------------------------------------------------- */
static void test_malloc_free_cycle(void) {
    int ok = 1;
    for (int i = 0; i < 100; i++) {
        void *p = malloc(256);
        if (!p) { ok = 0; break; }
        ((volatile char *)p)[0] = (char)i;
        free(p);
    }
    CHECK("malloc_free_cycle", ok);
}

/* -----------------------------------------------------------------------
 * 11. demand_paging_works: mmap large region, only touch 1 page --
 *     shouldn't consume all physical memory.
 * ----------------------------------------------------------------------- */
static void test_demand_paging_works(void) {
    long free_before = read_meminfo_kb("MemFree");

    /* Map 1024 pages (4 MB) */
    size_t sz = 1024 * 4096;
    char *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED || free_before < 0) {
        CHECK("demand_paging_works", p != MAP_FAILED);
        return;
    }

    /* Touch only one page */
    p[0] = 42;

    long free_after = read_meminfo_kb("MemFree");

    /*
     * If demand paging works, touching 1 page out of 1024 should not
     * consume anywhere near 4 MB.  We check that less than 512 kB was
     * consumed (generous margin for metadata overhead).
     */
    int ok = (free_before - free_after) < 512;
    munmap(p, sz);
    CHECK("demand_paging_works", ok);
}

/* ----------------------------------------------------------------------- */

int main(void) {
    printf("=== lazytest: lazy page allocation ===\n");

    test_sbrk_zero_returns_current();
    test_sbrk_grow_writable();
    test_sbrk_pages_zero_filled();
    test_sbrk_large_alloc();
    test_sbrk_touch_first_last();
    test_sbrk_random_access();
    test_mmap_anon_zero();
    test_mmap_anon_lazy();
    test_mmap_munmap_reuse();
    test_malloc_free_cycle();
    test_demand_paging_works();

    printf("=== lazytest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
