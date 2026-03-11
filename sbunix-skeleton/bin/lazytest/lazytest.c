#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>

static int tests_passed = 0;
static int tests_failed = 0;

static void check(int cond, const char *name) {
    if (cond) { printf("[PASS] lazy: %s\n", name); tests_passed++; }
    else { printf("[FAIL] lazy: %s\n", name); tests_failed++; }
}

int main(void) {
    unsigned long sz = 16 * 4096;
    char *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(p != MAP_FAILED, "mmap 64KB");

    p[0] = 'X';
    check(p[0] == 'X', "first page ok");

    p[sz - 1] = 'Y';
    check(p[sz - 1] == 'Y', "last page ok");

    /* zero init check (skip modified bytes) */
    int ok = 1;
    for (int i = 1; i < 100; i++) {
        if (p[i] != 0) { ok = 0; break; }
    }
    check(ok, "pages zero-init");

    /* small mmaps */
    for (int i = 0; i < 5; i++) {
        char *q = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check(q != MAP_FAILED, "small mmap");
        q[0] = (char)i;
    }

    int r = munmap(p, sz);
    check(r == 0, "munmap");

    printf("lazytest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
