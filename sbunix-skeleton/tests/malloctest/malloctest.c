/*
 * malloctest — malloc and explicit mmap allocation test
 *
 * Tests small malloc, page-sized malloc, large sbrk-based malloc,
 * calloc zero-fill, realloc data preservation, and explicit mmap
 * anonymous allocation.
 *
 * Run from the shell:  malloctest
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] malloctest: %s\n", label); g_pass++; } \
    else      { printf("[FAIL] malloctest: %s\n", label); g_fail++; } \
} while (0)

int main(void) {
  printf("=== malloctest: malloc and mmap allocation test ===\n");

  /* Test 1: Small malloc (128 bytes) */
  {
    char *p = malloc(128);
    CHECK("small malloc(128) succeeds", p != NULL);
    if (p) {
      memset(p, 'A', 128);
      CHECK("small malloc writable and readable", p[0] == 'A' && p[127] == 'A');
      free(p);
    }
  }

  /* Test 2: Page-sized malloc (4096 bytes) */
  {
    char *p = malloc(4096);
    CHECK("page-sized malloc(4096) succeeds", p != NULL);
    if (p) {
      memset(p, 'B', 4096);
      CHECK("page-sized malloc writable", p[0] == 'B' && p[4095] == 'B');
      free(p);
    }
  }

  /* Test 3: Large sbrk-based malloc (256KB) */
  {
    char *p = malloc(256 * 1024);
    CHECK("large malloc(256KB) succeeds", p != NULL);
    if (p) {
      /* Write pattern */
      for (int i = 0; i < 256 * 1024; i += 1024)
        p[i] = (char)(i >> 10);
      /* Verify */
      int ok = 1;
      for (int i = 0; i < 256 * 1024; i += 1024) {
        if (p[i] != (char)(i >> 10)) { ok = 0; break; }
      }
      CHECK("large malloc readback correct", ok);
      free(p);
    }
  }

  /* Test 4: calloc zero-fill */
  {
    int *p = calloc(1024, sizeof(int));
    CHECK("calloc(1024, 4) succeeds", p != NULL);
    if (p) {
      int all_zero = 1;
      for (int i = 0; i < 1024; i++) {
        if (p[i] != 0) { all_zero = 0; break; }
      }
      CHECK("calloc memory is zeroed", all_zero);
      free(p);
    }
  }

  /* Test 5: realloc with data preservation */
  {
    char *p = malloc(64);
    CHECK("realloc: initial malloc succeeds", p != NULL);
    if (p) {
      memcpy(p, "hello world!", 13);
      char *p2 = realloc(p, 256);
      CHECK("realloc to 256 succeeds", p2 != NULL);
      if (p2) {
        CHECK("realloc preserves data", memcmp(p2, "hello world!", 13) == 0);
        free(p2);
      }
    }
  }

  /* Test 6: Explicit mmap anonymous allocation (256KB) — directly
   * exercises mmap_find_hole() path.  libc malloc uses sbrk exclusively,
   * so large malloc does NOT test mmap. */
  {
    void *p = mmap(NULL, 256 * 1024, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK("explicit mmap(256KB) succeeds", p != MAP_FAILED);
    if (p != MAP_FAILED) {
      /* Write pattern */
      int *ip = (int *)p;
      for (int i = 0; i < 256 * 1024 / 4; i++)
        ip[i] = i;
      /* Verify */
      int ok = 1;
      for (int i = 0; i < 256 * 1024 / 4; i++) {
        if (ip[i] != i) { ok = 0; break; }
      }
      CHECK("explicit mmap readback correct", ok);
      munmap(p, 256 * 1024);
    }
  }

  printf("malloctest: %d passed, %d failed\n", g_pass, g_fail);
  return (g_fail > 0) ? 1 : 0;
}
