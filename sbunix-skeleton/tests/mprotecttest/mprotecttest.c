/*
 * mprotecttest -- verify mprotect works for file-backed mappings,
 * partial VMA splits, and anonymous metadata preservation.
 *
 * Tests:
 *  1. mprotect_file_backed: file mmap, PROT_NONE then PROT_READ, verify data
 *  2. mprotect_partial_vma: mprotect middle of large anon mapping
 *  3. mprotect_anon_metadata: anon mmap, write, RO then RW, verify data
 */

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

#define PAGE 4096

static int g_pass = 0;
static int g_fail = 0;

/* CHECK macro: prints [PASS]/[FAIL] with test name for host runner parsing */
#define CHECK(label, cond) do { \
    if (cond) { \
        printf("[PASS] mprotecttest: %s\n", label); \
        g_pass++; \
    } else { \
        printf("[FAIL] mprotecttest: %s\n", label); \
        g_fail++; \
    } \
} while (0)

/*
 * Test 1: mprotect_file_backed -- create test file on /mnt (ext2),
 * mmap it, mprotect to PROT_NONE then back to PROT_READ, verify
 * file data is intact after protection round-trip.
 */
static void test_mprotect_file_backed(void) {
  const char *path = "/mnt/mprotect_test_fb";
  char buf[PAGE];
  /* Fill with recognizable pattern */
  for (int i = 0; i < PAGE; i++) buf[i] = (char)(i & 0xFF);

  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd < 0) {
    CHECK("mprotect_file_backed", 0);
    return;
  }
  write(fd, buf, PAGE);
  close(fd);

  /* Reopen and mmap */
  fd = open(path, O_RDWR, 0);
  if (fd < 0) {
    CHECK("mprotect_file_backed", 0);
    unlink(path);
    return;
  }
  void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE, fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    CHECK("mprotect_file_backed", 0);
    unlink(path);
    return;
  }

  int ok = 1;

  /* mprotect to PROT_NONE (revoke all access) */
  int r = mprotect(p, PAGE, PROT_NONE);
  if (r != 0) ok = 0;

  /* mprotect back to PROT_READ */
  r = mprotect(p, PAGE, PROT_READ);
  if (r != 0) ok = 0;

  /* Verify file data is intact after protection round-trip */
  if (ok) {
    volatile unsigned char *c = (volatile unsigned char *)p;
    for (int i = 0; i < PAGE; i++) {
      if (c[i] != (unsigned char)(i & 0xFF)) { ok = 0; break; }
    }
  }

  munmap(p, PAGE);
  unlink(path);
  CHECK("mprotect_file_backed", ok);
}

/*
 * Test 2: mprotect_partial_vma -- mprotect the middle of a large
 * anonymous mapping, verify edges are unchanged (still writable).
 */
static void test_mprotect_partial_vma(void) {
  /* Allocate 4 pages */
  void *p = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("mprotect_partial_vma", 0);
    return;
  }

  /* Write pattern to all 4 pages */
  memset(p, 0xCC, 4 * PAGE);

  /* Make middle 2 pages (pages 1 and 2) read-only.
   * This should split the VMA into 3 parts. */
  int r = mprotect((char *)p + PAGE, 2 * PAGE, PROT_READ);
  int ok = (r == 0);

  if (ok) {
    /* Verify edge pages (0 and 3) are still writable */
    volatile unsigned char *c = (volatile unsigned char *)p;
    c[0] = 0xDD;                     /* page 0 write */
    c[3 * PAGE + PAGE - 1] = 0xEE;  /* page 3 write */
    ok = (c[0] == 0xDD && c[3 * PAGE + PAGE - 1] == 0xEE);
  }

  if (ok) {
    /* Verify middle pages still readable with original data */
    volatile unsigned char *c = (volatile unsigned char *)p;
    for (int i = PAGE; i < 3 * PAGE; i++) {
      if (c[i] != 0xCC) { ok = 0; break; }
    }
  }

  munmap(p, 4 * PAGE);
  CHECK("mprotect_partial_vma", ok);
}

/*
 * Test 3: mprotect_anon_metadata -- mmap anonymous, write data,
 * mprotect RO then back to RW, verify data is intact. This tests
 * that protection transitions preserve page contents.
 */
static void test_mprotect_anon_metadata(void) {
  void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("mprotect_anon_metadata", 0);
    return;
  }

  /* Write recognizable data */
  memset(p, 0xAB, PAGE);

  /* RW -> RO */
  int r1 = mprotect(p, PAGE, PROT_READ);
  /* RO -> RW */
  int r2 = mprotect(p, PAGE, PROT_READ | PROT_WRITE);

  int ok = (r1 == 0 && r2 == 0);

  if (ok) {
    /* Verify data survived the protection round-trip */
    unsigned char *c = (unsigned char *)p;
    for (int i = 0; i < PAGE; i++) {
      if (c[i] != 0xAB) { ok = 0; break; }
    }
  }

  /* Verify page is writable again after restoring RW */
  if (ok) {
    volatile unsigned char *c = (volatile unsigned char *)p;
    c[0] = 0xFE;
    ok = (c[0] == 0xFE);
  }

  munmap(p, PAGE);
  CHECK("mprotect_anon_metadata", ok);
}

int main(void) {
  printf("=== mprotecttest: mprotect test suite ===\n");

  test_mprotect_file_backed();
  test_mprotect_partial_vma();
  test_mprotect_anon_metadata();

  printf("\n=== mprotecttest: %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
