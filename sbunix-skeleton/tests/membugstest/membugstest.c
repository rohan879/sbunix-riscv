/*
 * membugstest -- Targeted regression tests for memory management bugs.
 *
 * Validates fixes for CoW isolation, VMA splitting, mapcount tracking,
 * fork stress, and partial unmap correctness.
 *
 * Tests:
 *  1. mprotect_cow_isolation      -- mprotect in child does not corrupt parent (B1)
 *  2. munmap_subrange_vma_order   -- munmap middle page preserves neighbors (B2)
 *  3. mprotect_split_cow_write    -- VMA split + CoW write in child succeeds (B3)
 *  4. cow_mapcount_survives_pressure -- CoW pages survive memory pressure (B4)
 *  5. cow_heavy_fork_stress       -- 20 forks writing 16 pages each (B5)
 *  6. uvmunmap_cow_partial        -- partial munmap after CoW preserves parent (B7)
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <stdlib.h>

#define PAGE 4096

static int g_pass = 0, g_fail = 0;

/* CHECK macro: prints [PASS]/[FAIL] with test name for host runner parsing */
#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] membugstest: %s\n", label); g_pass++; } \
    else { printf("[FAIL] membugstest: %s\n", label); g_fail++; } \
} while (0)

/* ===================================================================
 * 1. test_mprotect_cow_isolation (validates B1)
 *
 * After fork, the child calls mprotect then writes to a shared page.
 * The parent must still see its original data -- mprotect in the child
 * must not break CoW isolation.
 * =================================================================== */
static void test_mprotect_cow_isolation(void) {
  void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("mprotect_cow_isolation", 0);
    return;
  }

  /* Fill page with 0xCC pattern */
  memset(p, 0xCC, PAGE);

  pid_t pid = fork();
  if (pid < 0) {
    CHECK("mprotect_cow_isolation", 0);
    munmap(p, PAGE);
    return;
  }

  if (pid == 0) {
    /* Child: mprotect to RW (may trigger VMA changes), then write 0xDD */
    mprotect(p, PAGE, PROT_READ | PROT_WRITE);
    memset(p, 0xDD, PAGE);

    /* Verify child sees its own writes */
    volatile unsigned char *c = (volatile unsigned char *)p;
    int i;
    for (i = 0; i < PAGE; i++) {
      if (c[i] != 0xDD) _exit(1);
    }
    _exit(0);
  }

  /* Parent: wait for child, then verify our page is untouched */
  int status = 0;
  waitpid(pid, &status, 0);

  int ok = 1;
  volatile unsigned char *c = (volatile unsigned char *)p;
  int i;
  for (i = 0; i < PAGE; i++) {
    if (c[i] != 0xCC) { ok = 0; break; }
  }

  munmap(p, PAGE);
  CHECK("mprotect_cow_isolation", ok);
}

/* ===================================================================
 * 2. test_munmap_subrange_vma_order (validates B2)
 *
 * Map 3 contiguous pages, write distinct patterns, munmap the middle
 * page, and verify the first and third pages are still accessible
 * with correct data and remain writable.
 * =================================================================== */
static void test_munmap_subrange_vma_order(void) {
  void *p = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("munmap_subrange_vma_order", 0);
    return;
  }

  /* Write distinct patterns to each page */
  memset(p, 0xAA, PAGE);
  memset((char *)p + PAGE, 0xBB, PAGE);
  memset((char *)p + 2 * PAGE, 0xCC, PAGE);

  /* Unmap the middle page only */
  int r = munmap((char *)p + PAGE, PAGE);
  if (r != 0) {
    CHECK("munmap_subrange_vma_order", 0);
    munmap(p, 3 * PAGE);
    return;
  }

  int ok = 1;

  /* Verify first page still has 0xAA */
  volatile unsigned char *c = (volatile unsigned char *)p;
  int i;
  for (i = 0; i < PAGE; i++) {
    if (c[i] != 0xAA) { ok = 0; break; }
  }

  /* Verify third page still has 0xCC */
  if (ok) {
    volatile unsigned char *t = (volatile unsigned char *)((char *)p + 2 * PAGE);
    for (i = 0; i < PAGE; i++) {
      if (t[i] != 0xCC) { ok = 0; break; }
    }
  }

  /* Verify first and third pages are still writable */
  if (ok) {
    volatile unsigned char *first = (volatile unsigned char *)p;
    volatile unsigned char *third = (volatile unsigned char *)((char *)p + 2 * PAGE);
    first[0] = 0x11;
    third[0] = 0x22;
    if (first[0] != 0x11 || third[0] != 0x22) ok = 0;
  }

  /* Clean up remaining pages */
  munmap(p, PAGE);
  munmap((char *)p + 2 * PAGE, PAGE);
  CHECK("munmap_subrange_vma_order", ok);
}

/* ===================================================================
 * 3. test_mprotect_split_cow_write (validates B3)
 *
 * Map 4 pages, write known pattern, fork. In the child, mprotect
 * the middle 2 pages to RW (forces VMA split), write to them (triggers
 * CoW), verify data, and exit. Parent checks child exit status.
 * =================================================================== */
static void test_mprotect_split_cow_write(void) {
  void *p = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("mprotect_split_cow_write", 0);
    return;
  }

  /* Write known pattern to all 4 pages */
  memset(p, 0xEE, 4 * PAGE);

  pid_t pid = fork();
  if (pid < 0) {
    CHECK("mprotect_split_cow_write", 0);
    munmap(p, 4 * PAGE);
    return;
  }

  if (pid == 0) {
    /* Child: mprotect middle 2 pages to RW (forces VMA split) */
    int r = mprotect((char *)p + PAGE, 2 * PAGE, PROT_READ | PROT_WRITE);
    if (r != 0) _exit(1);

    /* Write to the split region (triggers CoW) */
    memset((char *)p + PAGE, 0xFF, 2 * PAGE);

    /* Verify the written data */
    volatile unsigned char *c = (volatile unsigned char *)((char *)p + PAGE);
    int i;
    for (i = 0; i < 2 * PAGE; i++) {
      if (c[i] != 0xFF) _exit(2);
    }

    /* Verify edge pages still have original pattern */
    volatile unsigned char *first = (volatile unsigned char *)p;
    volatile unsigned char *last = (volatile unsigned char *)((char *)p + 3 * PAGE);
    for (i = 0; i < PAGE; i++) {
      if (first[i] != 0xEE) _exit(3);
      if (last[i] != 0xEE) _exit(4);
    }
    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);

  munmap(p, 4 * PAGE);
  CHECK("mprotect_split_cow_write",
        WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* ===================================================================
 * 4. test_cow_mapcount_survives_pressure (validates B4)
 *
 * Map 4 pages, write pattern, fork. Child writes different pattern
 * (CoW), then allocates 200 more pages (memory pressure), verifies
 * its pages survived. Parent verifies its data is intact.
 * =================================================================== */
static void test_cow_mapcount_survives_pressure(void) {
  void *p = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("cow_mapcount_survives_pressure", 0);
    return;
  }

  /* Write 0x42 pattern to all 4 pages */
  memset(p, 0x42, 4 * PAGE);

  pid_t pid = fork();
  if (pid < 0) {
    CHECK("cow_mapcount_survives_pressure", 0);
    munmap(p, 4 * PAGE);
    return;
  }

  if (pid == 0) {
    /* Child: write different pattern to trigger CoW on all 4 pages */
    memset(p, 0x99, 4 * PAGE);

    /* Allocate 200 more pages to create memory pressure */
    void *pressure = mmap(NULL, 200 * PAGE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pressure == MAP_FAILED) _exit(1);

    /* Touch all pressure pages to ensure they are allocated */
    memset(pressure, 0x55, 200 * PAGE);

    /* Verify original 4 pages still have child's pattern */
    volatile unsigned char *c = (volatile unsigned char *)p;
    int i;
    for (i = 0; i < 4 * PAGE; i++) {
      if (c[i] != 0x99) _exit(2);
    }

    munmap(pressure, 200 * PAGE);
    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);

  /* Parent: verify our pages still have 0x42 */
  int parent_ok = 1;
  volatile unsigned char *c = (volatile unsigned char *)p;
  int i;
  for (i = 0; i < 4 * PAGE; i++) {
    if (c[i] != 0x42) { parent_ok = 0; break; }
  }

  int child_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

  munmap(p, 4 * PAGE);
  CHECK("cow_mapcount_survives_pressure", parent_ok && child_ok);
}

/* ===================================================================
 * 5. test_cow_heavy_fork_stress (validates B5)
 *
 * Map 16 pages, write known pattern, fork 20 children that each write
 * to all 16 pages. Parent waits for all children and verifies its
 * data is intact.
 * =================================================================== */
static void test_cow_heavy_fork_stress(void) {
  const int NPAGES = 16;
  const int NCHILDREN = 20;

  void *p = mmap(NULL, NPAGES * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("cow_heavy_fork_stress", 0);
    return;
  }

  /* Write known pattern */
  memset(p, 0x37, NPAGES * PAGE);

  pid_t pids[20];
  int i;
  int all_forked = 1;

  for (i = 0; i < NCHILDREN; i++) {
    pids[i] = fork();
    if (pids[i] < 0) {
      all_forked = 0;
      break;
    }
    if (pids[i] == 0) {
      /* Child: write unique pattern to all pages */
      unsigned char pattern = (unsigned char)(0x40 + i);
      memset(p, pattern, NPAGES * PAGE);

      /* Verify child sees its own writes */
      volatile unsigned char *c = (volatile unsigned char *)p;
      int j;
      for (j = 0; j < NPAGES * PAGE; j++) {
        if (c[j] != pattern) _exit(1);
      }
      _exit(0);
    }
  }

  /* Wait for all children that were successfully forked */
  int all_ok = all_forked;
  int forked_count = all_forked ? NCHILDREN : i;
  for (i = 0; i < forked_count; i++) {
    int status = 0;
    waitpid(pids[i], &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
      all_ok = 0;
  }

  /* Verify parent's data is intact */
  int parent_ok = 1;
  volatile unsigned char *c = (volatile unsigned char *)p;
  int j;
  for (j = 0; j < NPAGES * PAGE; j++) {
    if (c[j] != 0x37) { parent_ok = 0; break; }
  }

  munmap(p, NPAGES * PAGE);
  CHECK("cow_heavy_fork_stress", all_ok && parent_ok);
}

/* ===================================================================
 * 6. test_uvmunmap_cow_partial (validates B7)
 *
 * Map 4 pages, write pattern, fork. Child writes different pattern
 * to first 2 pages (CoW), then munmaps last 2 pages, verifies first
 * 2 are intact. Parent verifies all 4 pages still have original data.
 * =================================================================== */
static void test_uvmunmap_cow_partial(void) {
  void *p = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("uvmunmap_cow_partial", 0);
    return;
  }

  /* Write 0xAB pattern to all 4 pages */
  memset(p, 0xAB, 4 * PAGE);

  pid_t pid = fork();
  if (pid < 0) {
    CHECK("uvmunmap_cow_partial", 0);
    munmap(p, 4 * PAGE);
    return;
  }

  if (pid == 0) {
    /* Child: write different pattern to first 2 pages (triggers CoW) */
    memset(p, 0xCD, 2 * PAGE);

    /* Munmap last 2 pages */
    int r = munmap((char *)p + 2 * PAGE, 2 * PAGE);
    if (r != 0) _exit(1);

    /* Verify first 2 pages still have child's pattern */
    volatile unsigned char *c = (volatile unsigned char *)p;
    int i;
    for (i = 0; i < 2 * PAGE; i++) {
      if (c[i] != 0xCD) _exit(2);
    }
    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);

  /* Parent: verify all 4 pages still have 0xAB */
  int parent_ok = 1;
  volatile unsigned char *c = (volatile unsigned char *)p;
  int i;
  for (i = 0; i < 4 * PAGE; i++) {
    if (c[i] != 0xAB) { parent_ok = 0; break; }
  }

  int child_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

  munmap(p, 4 * PAGE);
  CHECK("uvmunmap_cow_partial", parent_ok && child_ok);
}

/* =================================================================== */

int main(void) {
  printf("membugstest: starting 6 tests\n");
  test_mprotect_cow_isolation();
  test_munmap_subrange_vma_order();
  test_mprotect_split_cow_write();
  test_cow_mapcount_survives_pressure();
  test_cow_heavy_fork_stress();
  test_uvmunmap_cow_partial();
  printf("membugstest: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail > 0 ? 1 : 0;
}
