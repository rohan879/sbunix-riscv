#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int tests_passed = 0;
static int tests_failed = 0;

static void check(int cond, const char *name) {
    if (cond) { printf("[PASS] lazy: %s\n", name); tests_passed++; }
    else { printf("[FAIL] lazy: %s\n", name); tests_failed++; }
}

int main(void) {
    /* Basic lazy mmap */
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

    /* Large mmap — 1MB, only touch a few pages (lazy) */
    unsigned long big = 256 * 4096; /* 1MB */
    char *lp = mmap(NULL, big, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(lp != MAP_FAILED, "mmap 1MB");
    lp[0] = 'A';
    lp[big / 2] = 'B';
    lp[big - 1] = 'C';
    check(lp[0] == 'A' && lp[big / 2] == 'B' && lp[big - 1] == 'C',
          "large mmap sparse access");
    munmap(lp, big);

    /* mmap + fork — child inherits lazy mapping */
    sz = 4 * 4096;
    p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(p != MAP_FAILED, "mmap for fork");
    p[0] = 42;

    pid_t pid = fork();
    if (pid == 0) {
        /* child: read inherited page, write new page */
        if (p[0] != 42) _exit(1);
        p[4096] = 99; /* trigger COW or lazy fault in child */
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "fork+mmap child ok");
    /* parent's page should be unchanged (COW) */
    check(p[4096] == 0, "fork COW isolation");
    munmap(p, sz);

    /* mmap MAP_FAILED on invalid args */
    char *bad = mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(bad == MAP_FAILED, "mmap size=0 fails");

    printf("lazytest: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
