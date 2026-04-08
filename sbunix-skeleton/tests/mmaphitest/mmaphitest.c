/*
 * mmaphitest — mmap high-VA collision regression test
 *
 * Verifies that MAP_FIXED at 0x30000000 (PCIe ECAM range) succeeds
 * and returns zeroed writable memory, and that repeated mmap
 * allocations above 0x30000000 are usable.
 *
 * Run from the shell:  mmaphitest
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] mmaphitest: %s\n", label); g_pass++; } \
    else      { printf("[FAIL] mmaphitest: %s\n", label); g_fail++; } \
} while (0)

int main(void) {
  printf("=== mmaphitest: mmap high-VA collision test ===\n");

  /* Test 1: MAP_FIXED at 0x30000000 (PCIe ECAM range) — must succeed
   * and return zeroed writable memory.  Before the MMIO L1 filter fix,
   * this would crash because kernel superpages blocked demand-paging. */
  {
    void *addr = (void *)0x30000000UL;
    void *p = mmap(addr, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK("MAP_FIXED at 0x30000000 succeeds", p == addr);
    if (p == addr) {
      /* Verify zeroed memory */
      unsigned char *c = (unsigned char *)p;
      int all_zero = 1;
      for (int i = 0; i < 4096; i++) {
        if (c[i] != 0) { all_zero = 0; break; }
      }
      CHECK("MAP_FIXED 0x30000000 memory is zeroed", all_zero);

      /* Verify writable */
      c[0] = 0xAB;
      c[4095] = 0xCD;
      CHECK("MAP_FIXED 0x30000000 memory is writable",
            c[0] == 0xAB && c[4095] == 0xCD);

      munmap(p, 4096);
    }
  }

  /* Test 2: Repeated 2MB mmap allocations to push mmap_base above
   * 0x30000000 — all must be usable. */
  {
    int ok = 1;
    int count = 0;
    void *ptrs[20];
    for (int i = 0; i < 20; i++) {
      ptrs[i] = mmap(NULL, 2 * 1024 * 1024, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (ptrs[i] == MAP_FAILED) {
        ok = 0;
        break;
      }
      /* Write to first and last page to verify usability */
      volatile char *c = (volatile char *)ptrs[i];
      c[0] = (char)i;
      c[2 * 1024 * 1024 - 1] = (char)(i + 1);
      if (c[0] != (char)i || c[2 * 1024 * 1024 - 1] != (char)(i + 1)) {
        ok = 0;
        break;
      }
      count++;
    }
    CHECK("repeated 2MB mmap allocations all usable", ok && count == 20);

    /* Cleanup */
    for (int i = 0; i < count; i++)
      munmap(ptrs[i], 2 * 1024 * 1024);
  }

  /* Test 3: Readback verification of high-VA allocations */
  {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK("high-VA anonymous mmap succeeds", p != MAP_FAILED);
    if (p != MAP_FAILED) {
      /* Write a pattern and read back */
      int *ip = (int *)p;
      for (int i = 0; i < 1024; i++)
        ip[i] = i * 7;
      int ok = 1;
      for (int i = 0; i < 1024; i++) {
        if (ip[i] != i * 7) { ok = 0; break; }
      }
      CHECK("high-VA readback correct", ok);
      munmap(p, 4096);
    }
  }

  printf("mmaphitest: %d passed, %d failed\n", g_pass, g_fail);
  return (g_fail > 0) ? 1 : 0;
}
