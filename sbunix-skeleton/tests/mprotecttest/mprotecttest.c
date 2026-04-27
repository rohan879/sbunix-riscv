/*
 * mprotecttest -- verify mprotect works for file-backed mappings,
 * partial VMA splits, and anonymous metadata preservation.
 *
 * Tests:
 *  1. mprotect_file_backed: file mmap, PROT_NONE then PROT_READ, verify data
 *  2. mprotect_partial_vma: mprotect middle of large anon mapping
 *  3. mprotect_anon_metadata: anon mmap, write, RO then RW, verify data
 *  4. mprotect_syscall_usercopy: syscalls cannot bypass PROT_NONE
 *  5. mprotect_file_max_prot: mprotect cannot exceed fd-derived access
 *  6. filebacked_lazy_syscall_buffers: copy helpers fault file pages in
 */

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>

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

/*
 * Test 4: mprotect_syscall_usercopy -- PROT_NONE must block supervisor
 * copyin/copyout helpers as well as ordinary user-mode loads/stores.
 */
static void test_mprotect_syscall_usercopy(void) {
  void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("mprotect_syscall_usercopy", 0);
    return;
  }

  memset(p, 'X', PAGE);
  int ok = (mprotect(p, PAGE, PROT_NONE) == 0);

  errno = 0;
  ssize_t wr = write(1, p, 1);
  if (!(wr == -1 && errno == EFAULT))
    ok = 0;

  char *cwd = getcwd((char *)p, PAGE);
  if (cwd != NULL)
    ok = 0;

  if (mprotect(p, PAGE, PROT_READ | PROT_WRITE) != 0)
    ok = 0;
  if (ok) {
    ((char *)p)[0] = 'Y';
    ok = ((char *)p)[0] == 'Y';
  }

  munmap(p, PAGE);
  CHECK("mprotect_syscall_usercopy", ok);
}

/*
 * Test 5: mprotect_file_max_prot -- a MAP_SHARED mapping from an O_RDONLY
 * fd must not become writable through mprotect().
 */
static void test_mprotect_file_max_prot(void) {
  const char *path = "/mnt/mprotect_test_maxprot";
  char buf[PAGE];
  memset(buf, 0x5A, PAGE);

  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd < 0) {
    CHECK("mprotect_file_max_prot", 0);
    return;
  }
  if (write(fd, buf, PAGE) != PAGE) {
    close(fd);
    unlink(path);
    CHECK("mprotect_file_max_prot", 0);
    return;
  }
  close(fd);

  fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    unlink(path);
    CHECK("mprotect_file_max_prot", 0);
    return;
  }
  char *p = mmap(NULL, PAGE, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    unlink(path);
    CHECK("mprotect_file_max_prot", 0);
    return;
  }

  errno = 0;
  int r = mprotect(p, PAGE, PROT_READ | PROT_WRITE);
  int ok = (r < 0 && errno == EACCES);
  if (ok)
    ok = ((unsigned char *)p)[0] == 0x5A;

  munmap(p, PAGE);
  unlink(path);
  CHECK("mprotect_file_max_prot", ok);
}

static int write_file(const char *path, const void *data, int len) {
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd < 0)
    return -1;
  int ok = (write(fd, data, len) == len);
  fsync(fd);
  close(fd);
  return ok ? 0 : -1;
}

/*
 * Test 6: filebacked_lazy_syscall_buffers -- syscall copyin/copyout and
 * copyinstr must demand-fault file-backed private mappings, not just
 * anonymous lazy pages.
 */
static void test_filebacked_lazy_syscall_buffers(void) {
  const char *data_path = "/mnt/mprotect_lazy_data";
  const char *path_path = "/mnt/mprotect_lazy_path";
  const char *payload_path = "/mnt/mprotect_lazy_payload";
  const char msg[] = "lazy-file-backed-buffer";
  char page[PAGE];
  int pfd[2] = {-1, -1};
  int ok = 1;

  memset(page, 0, sizeof(page));
  memcpy(page, msg, sizeof(msg));
  if (write_file(data_path, page, PAGE) < 0)
    ok = 0;
  if (write_file(payload_path, "payload", 7) < 0)
    ok = 0;
  if (write_file(path_path, payload_path, (int)strlen(payload_path) + 1) < 0)
    ok = 0;

  int fd = ok ? open(data_path, O_RDONLY, 0) : -1;
  char *src = MAP_FAILED;
  if (fd >= 0) {
    src = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
  }
  if (src == MAP_FAILED)
    ok = 0;

  if (ok && pipe(pfd) < 0)
    ok = 0;
  if (ok) {
    ssize_t nw = write(pfd[1], src, sizeof(msg));
    char verify[64];
    memset(verify, 0, sizeof(verify));
    ssize_t nr = read(pfd[0], verify, sizeof(msg));
    ok = (nw == (ssize_t)sizeof(msg) &&
          nr == (ssize_t)sizeof(msg) &&
          memcmp(verify, msg, sizeof(msg)) == 0);
  }
  if (pfd[0] >= 0) close(pfd[0]);
  if (pfd[1] >= 0) close(pfd[1]);
  pfd[0] = pfd[1] = -1;
  if (src != MAP_FAILED)
    munmap(src, PAGE);

  fd = ok ? open(data_path, O_RDONLY, 0) : -1;
  char *dst = MAP_FAILED;
  if (fd >= 0) {
    dst = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);
  }
  if (dst == MAP_FAILED)
    ok = 0;

  if (ok && pipe(pfd) < 0)
    ok = 0;
  if (ok) {
    ssize_t nw = write(pfd[1], msg, sizeof(msg));
    ssize_t nr = read(pfd[0], dst, sizeof(msg));
    ok = (nw == (ssize_t)sizeof(msg) &&
          nr == (ssize_t)sizeof(msg) &&
          memcmp(dst, msg, sizeof(msg)) == 0);
  }
  if (pfd[0] >= 0) close(pfd[0]);
  if (pfd[1] >= 0) close(pfd[1]);
  pfd[0] = pfd[1] = -1;
  if (dst != MAP_FAILED)
    munmap(dst, PAGE);

  fd = ok ? open(path_path, O_RDONLY, 0) : -1;
  char *lazy_path = MAP_FAILED;
  if (fd >= 0) {
    lazy_path = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
  }
  if (lazy_path == MAP_FAILED)
    ok = 0;
  if (ok) {
    int payload_fd = open(lazy_path, O_RDONLY, 0);
    ok = (payload_fd >= 0);
    if (payload_fd >= 0)
      close(payload_fd);
  }
  if (lazy_path != MAP_FAILED)
    munmap(lazy_path, PAGE);

  unlink(data_path);
  unlink(path_path);
  unlink(payload_path);
  CHECK("filebacked_lazy_syscall_buffers", ok);
}

int main(void) {
  printf("=== mprotecttest: mprotect test suite ===\n");

  test_mprotect_file_backed();
  test_mprotect_partial_vma();
  test_mprotect_anon_metadata();
  test_mprotect_syscall_usercopy();
  test_mprotect_file_max_prot();
  test_filebacked_lazy_syscall_buffers();

  printf("\n=== mprotecttest: %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
