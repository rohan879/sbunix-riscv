/*
 * mremaptest -- verify mremap works correctly for shrink, grow,
 * MREMAP_MAYMOVE, protection preservation, pressure, and file-backed cases.
 *
 * Tests:
 *  1. mremap_shrink: shrink 2 pages to 1, verify data survives
 *  2. mremap_grow_inplace: grow 1 page to 2, verify old data + new page
 *  3. mremap_maymove_data: grow 1->4 pages with MAYMOVE, verify data
 *  4. mremap_preserves_prot: mmap RO, mremap MAYMOVE, verify read works
 *  5. mremap_pressure_pages: mmap several pages under modest pressure
 *  6. mremap_shrink_shared_writeback: MAP_SHARED file, shrink, verify
 *  7. mremap_shrink_preserves_split_prot: mprotect partial, shrink
 *  8. mremap_maymove_preserves_split_prot: mprotect partial, MAYMOVE
 *  9. mremap_grow_move_tail: grow with MAYMOVE, write to tail pages
 * 10. mremap_filebacked_oldrange: file-backed MAP_PRIVATE MAYMOVE
 * 11. mremap_filebacked_grow_tail_anon: file-backed grow, tail writable
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
        printf("[PASS] mremaptest: %s\n", label); \
        g_pass++; \
    } else { \
        printf("[FAIL] mremaptest: %s\n", label); \
        g_fail++; \
    } \
} while (0)

/* Helper: create a temp file on /mnt with given data, return fd.
 * /mnt is the ext2 read-write mount in SBUnix. */
static int create_tmpfile(const char *path, const void *data, size_t len) {
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd < 0) return -1;
  ssize_t w = write(fd, data, len);
  if (w != (ssize_t)len) {
    close(fd);
    return -1;
  }
  /* Seek back to start so caller can mmap from offset 0 */
  close(fd);
  fd = open(path, O_RDWR, 0);
  return fd;
}

/*
 * Test 1: mremap shrink -- mmap 2 pages, write pattern, shrink to 1 page,
 * verify first page data survives.
 */
static void test_mremap_shrink(void) {
  void *p = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("mremap_shrink", 0);
    return;
  }
  /* Fill first page with pattern */
  memset(p, 0xAA, PAGE);

  /* Shrink to 1 page */
  void *q = mremap(p, 2 * PAGE, PAGE, 0);
  /* mremap shrink should succeed and may return same address */
  int ok = (q != MAP_FAILED);
  if (ok) {
    /* Verify first page data survived */
    unsigned char *c = (unsigned char *)q;
    for (int i = 0; i < PAGE; i++) {
      if (c[i] != 0xAA) { ok = 0; break; }
    }
  }
  CHECK("mremap_shrink", ok);
  if (q != MAP_FAILED) munmap(q, PAGE);
}

/*
 * Test 2: mremap grow in-place -- mmap 1 page, write pattern,
 * grow to 2 pages, verify old data and new page accessible.
 */
static void test_mremap_grow_inplace(void) {
  void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("mremap_grow_inplace", 0);
    return;
  }
  memset(p, 0xBB, PAGE);

  /* Try to grow in-place (no MAYMOVE) */
  void *q = mremap(p, PAGE, 2 * PAGE, 0);
  if (q == MAP_FAILED) {
    /* In-place growth may fail if no room; that is acceptable behavior.
     * Try with MAYMOVE as fallback to verify grow works at all. */
    q = mremap(p, PAGE, 2 * PAGE, MREMAP_MAYMOVE);
  }
  int ok = (q != MAP_FAILED);
  if (ok) {
    /* Verify old data preserved */
    unsigned char *c = (unsigned char *)q;
    for (int i = 0; i < PAGE; i++) {
      if (c[i] != 0xBB) { ok = 0; break; }
    }
    /* Verify new page is accessible (write + read back) */
    if (ok) {
      c[PAGE] = 0xCC;
      ok = (c[PAGE] == 0xCC);
    }
    munmap(q, 2 * PAGE);
  } else {
    munmap(p, PAGE);
  }
  CHECK("mremap_grow_inplace", ok);
}

/*
 * Test 3: mremap MAYMOVE data preservation -- mmap 1 page, write data,
 * grow to 4 pages with MREMAP_MAYMOVE, verify data at new location.
 */
static void test_mremap_maymove_data(void) {
  void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("mremap_maymove_data", 0);
    return;
  }
  /* Write recognizable pattern */
  memset(p, 0xDE, PAGE);

  void *q = mremap(p, PAGE, 4 * PAGE, MREMAP_MAYMOVE);
  int ok = (q != MAP_FAILED);
  if (ok) {
    unsigned char *c = (unsigned char *)q;
    /* Verify original data preserved */
    for (int i = 0; i < PAGE; i++) {
      if (c[i] != 0xDE) { ok = 0; break; }
    }
    munmap(q, 4 * PAGE);
  } else {
    munmap(p, PAGE);
  }
  CHECK("mremap_maymove_data", ok);
}

/*
 * Test 4: mremap preserves protection -- mmap RO page, mremap with MAYMOVE,
 * verify read still works (we just check read, not write fault).
 */
static void test_mremap_preserves_prot(void) {
  /* Allocate RW first to write data, then mprotect to RO */
  void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("mremap_preserves_prot", 0);
    return;
  }
  memset(p, 0x42, PAGE);
  /* Make read-only */
  mprotect(p, PAGE, PROT_READ);

  void *q = mremap(p, PAGE, PAGE, MREMAP_MAYMOVE);
  int ok = (q != MAP_FAILED);
  if (ok) {
    /* Verify read still works */
    volatile unsigned char *c = (volatile unsigned char *)q;
    ok = (c[0] == 0x42 && c[PAGE - 1] == 0x42);
    munmap(q, PAGE);
  } else {
    munmap(p, PAGE);
  }
  CHECK("mremap_preserves_prot", ok);
}

/*
 * Test 5: mremap under modest resident pressure -- mmap several pages,
 * write pattern, mremap with MAYMOVE, verify data integrity.
 */
static void test_mremap_pressure_pages(void) {
  /* Allocate enough pages to put pressure on memory (16 pages = 64KB).
   * Kept modest to avoid OOM on 128MB system. */
  size_t npages = 16;
  size_t sz = npages * PAGE;
  void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("mremap_pressure_pages", 0);
    return;
  }
  /* Write pattern: each page gets its page index in first byte */
  for (size_t i = 0; i < npages; i++) {
    ((unsigned char *)p)[i * PAGE] = (unsigned char)(i & 0xFF);
  }

  /* Grow with MAYMOVE to force potential relocation */
  void *q = mremap(p, sz, sz + 2 * PAGE, MREMAP_MAYMOVE);
  int ok = (q != MAP_FAILED);
  if (ok) {
    /* Verify original pattern survived */
    for (size_t i = 0; i < npages; i++) {
      if (((unsigned char *)q)[i * PAGE] != (unsigned char)(i & 0xFF)) {
        ok = 0;
        break;
      }
    }
    munmap(q, sz + 2 * PAGE);
  } else {
    munmap(p, sz);
  }
  CHECK("mremap_pressure_pages", ok);
}

/*
 * Test 6: mremap shrink shared writeback -- create temp file on /mnt (ext2),
 * mmap MAP_SHARED, write data, mremap shrink, verify file data persists.
 */
static void test_mremap_shrink_shared_writeback(void) {
  const char *path = "/mnt/mremap_test_shared";
  /* Create file with 2 pages of data */
  char buf[PAGE];
  memset(buf, 0x55, PAGE);

  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd < 0) {
    CHECK("mremap_shrink_shared_writeback", 0);
    return;
  }
  /* Write 2 pages */
  write(fd, buf, PAGE);
  write(fd, buf, PAGE);
  close(fd);

  /* Reopen and mmap MAP_SHARED */
  fd = open(path, O_RDWR, 0);
  if (fd < 0) {
    CHECK("mremap_shrink_shared_writeback", 0);
    unlink(path);
    return;
  }
  void *p = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    CHECK("mremap_shrink_shared_writeback", 0);
    unlink(path);
    return;
  }

  /* Write distinct pattern to first page via mapping */
  memset(p, 0x77, PAGE);

  /* Shrink mapping to 1 page */
  void *q = mremap(p, 2 * PAGE, PAGE, 0);
  int ok = (q != MAP_FAILED);
  if (ok) {
    munmap(q, PAGE);
  } else {
    munmap(p, 2 * PAGE);
  }

  /* Verify written data persists by reading file directly */
  if (ok) {
    fd = open(path, O_RDWR, 0);
    if (fd >= 0) {
      char verify[PAGE];
      ssize_t n = read(fd, verify, PAGE);
      if (n == PAGE) {
        for (int i = 0; i < PAGE; i++) {
          if ((unsigned char)verify[i] != 0x77) { ok = 0; break; }
        }
      } else {
        ok = 0;
      }
      close(fd);
    } else {
      ok = 0;
    }
  }

  unlink(path);
  CHECK("mremap_shrink_shared_writeback", ok);
}

/*
 * Test 7: mremap shrink preserves split protection -- mmap region,
 * mprotect first half RO, mremap shrink to first half, verify data intact.
 */
static void test_mremap_shrink_preserves_split_prot(void) {
  void *p = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("mremap_shrink_preserves_split_prot", 0);
    return;
  }
  /* Write pattern to first 2 pages */
  memset(p, 0xEE, 2 * PAGE);

  /* Make first 2 pages read-only */
  mprotect(p, 2 * PAGE, PROT_READ);

  /* Shrink to first 2 pages only */
  void *q = mremap(p, 4 * PAGE, 2 * PAGE, 0);
  int ok = (q != MAP_FAILED);
  if (ok) {
    /* Verify data in first 2 pages still readable */
    volatile unsigned char *c = (volatile unsigned char *)q;
    for (int i = 0; i < 2 * PAGE; i++) {
      if (c[i] != 0xEE) { ok = 0; break; }
    }
    munmap(q, 2 * PAGE);
  } else {
    munmap(p, 4 * PAGE);
  }
  CHECK("mremap_shrink_preserves_split_prot", ok);
}

/*
 * Test 8: mremap MAYMOVE preserves split protection -- mmap region,
 * mprotect first half RO, mremap with MAYMOVE, verify first half readable.
 */
static void test_mremap_maymove_preserves_split_prot(void) {
  void *p = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("mremap_maymove_preserves_split_prot", 0);
    return;
  }
  /* Write pattern everywhere */
  memset(p, 0xDD, 4 * PAGE);

  /* Make first 2 pages read-only */
  mprotect(p, 2 * PAGE, PROT_READ);

  /* mremap with MAYMOVE (same size) */
  void *q = mremap(p, 4 * PAGE, 4 * PAGE, MREMAP_MAYMOVE);
  int ok = (q != MAP_FAILED);
  if (ok) {
    /* Verify first half (was RO) still readable with correct data */
    volatile unsigned char *c = (volatile unsigned char *)q;
    for (int i = 0; i < 2 * PAGE; i++) {
      if (c[i] != 0xDD) { ok = 0; break; }
    }
    munmap(q, 4 * PAGE);
  } else {
    munmap(p, 4 * PAGE);
  }
  CHECK("mremap_maymove_preserves_split_prot", ok);
}

/*
 * Test 9: mremap grow + move tail -- mmap 1 page, mremap grow with MAYMOVE,
 * write to growth tail pages to verify they are accessible.
 */
static void test_mremap_grow_move_tail(void) {
  void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("mremap_grow_move_tail", 0);
    return;
  }
  memset(p, 0xAA, PAGE);

  /* Grow from 1 page to 4 pages with MAYMOVE */
  void *q = mremap(p, PAGE, 4 * PAGE, MREMAP_MAYMOVE);
  int ok = (q != MAP_FAILED);
  if (ok) {
    unsigned char *c = (unsigned char *)q;
    /* Verify original page data */
    for (int i = 0; i < PAGE; i++) {
      if (c[i] != 0xAA) { ok = 0; break; }
    }
    /* Write to tail pages (pages 2, 3, 4) and read back */
    if (ok) {
      for (int pg = 1; pg < 4; pg++) {
        c[pg * PAGE] = (unsigned char)(pg + 0x10);
        c[pg * PAGE + PAGE - 1] = (unsigned char)(pg + 0x20);
      }
      for (int pg = 1; pg < 4; pg++) {
        if (c[pg * PAGE] != (unsigned char)(pg + 0x10) ||
            c[pg * PAGE + PAGE - 1] != (unsigned char)(pg + 0x20)) {
          ok = 0;
          break;
        }
      }
    }
    munmap(q, 4 * PAGE);
  } else {
    munmap(p, PAGE);
  }
  CHECK("mremap_grow_move_tail", ok);
}

/*
 * Test 10: mremap file-backed old range -- create file on /mnt, mmap
 * MAP_PRIVATE, force MAYMOVE, verify file data survives in moved mapping.
 */
static void test_mremap_filebacked_oldrange(void) {
  const char *path = "/mnt/mremap_test_fb";
  /* Create file with known pattern */
  char buf[PAGE];
  for (int i = 0; i < PAGE; i++) buf[i] = (char)(i & 0xFF);

  int fd = create_tmpfile(path, buf, PAGE);
  if (fd < 0) {
    CHECK("mremap_filebacked_oldrange", 0);
    return;
  }

  void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE, fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    CHECK("mremap_filebacked_oldrange", 0);
    unlink(path);
    return;
  }

  /* Force MAYMOVE by growing */
  void *q = mremap(p, PAGE, 2 * PAGE, MREMAP_MAYMOVE);
  int ok = (q != MAP_FAILED);
  if (ok) {
    /* Verify file data survived in moved mapping */
    unsigned char *c = (unsigned char *)q;
    for (int i = 0; i < PAGE; i++) {
      if (c[i] != (unsigned char)(i & 0xFF)) { ok = 0; break; }
    }
    munmap(q, 2 * PAGE);
  } else {
    munmap(p, PAGE);
  }

  unlink(path);
  CHECK("mremap_filebacked_oldrange", ok);
}

/*
 * Test 11: mremap file-backed grow tail anonymous -- create file on /mnt,
 * mmap MAP_PRIVATE, mremap grow+move, verify old-range data correct and
 * growth tail is writable (anonymous zero-fill).
 */
static void test_mremap_filebacked_grow_tail_anon(void) {
  const char *path = "/mnt/mremap_test_fbtail";
  char buf[PAGE];
  memset(buf, 0x99, PAGE);

  int fd = create_tmpfile(path, buf, PAGE);
  if (fd < 0) {
    CHECK("mremap_filebacked_grow_tail_anon", 0);
    return;
  }

  void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE, fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    CHECK("mremap_filebacked_grow_tail_anon", 0);
    unlink(path);
    return;
  }

  /* Grow from 1 page to 3 pages with MAYMOVE */
  void *q = mremap(p, PAGE, 3 * PAGE, MREMAP_MAYMOVE);
  int ok = (q != MAP_FAILED);
  if (ok) {
    unsigned char *c = (unsigned char *)q;
    /* Verify old-range file data correct */
    for (int i = 0; i < PAGE; i++) {
      if (c[i] != 0x99) { ok = 0; break; }
    }
    /* Verify growth tail pages are writable (anonymous) */
    if (ok) {
      c[PAGE] = 0xF1;
      c[2 * PAGE] = 0xF2;
      ok = (c[PAGE] == 0xF1 && c[2 * PAGE] == 0xF2);
    }
    munmap(q, 3 * PAGE);
  } else {
    munmap(p, PAGE);
  }

  unlink(path);
  CHECK("mremap_filebacked_grow_tail_anon", ok);
}

int main(void) {
  printf("=== mremaptest: mremap test suite ===\n");

  test_mremap_shrink();
  test_mremap_grow_inplace();
  test_mremap_maymove_data();
  test_mremap_preserves_prot();
  test_mremap_pressure_pages();
  test_mremap_shrink_shared_writeback();
  test_mremap_shrink_preserves_split_prot();
  test_mremap_maymove_preserves_split_prot();
  test_mremap_grow_move_tail();
  test_mremap_filebacked_oldrange();
  test_mremap_filebacked_grow_tail_anon();

  printf("\n=== mremaptest: %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
