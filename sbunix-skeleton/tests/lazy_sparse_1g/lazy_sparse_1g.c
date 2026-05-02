/*
 * lazy_sparse_1g -- a 1 GiB mmap reservation must stay sparse.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#define PAGE_SIZE_BYTES 4096UL
#define ONE_GIB (1024UL * 1024UL * 1024UL)

static long read_memfree_kb(void) {
  int fd = open("/proc/meminfo", O_RDONLY);
  char buf[1024];
  int n;
  char *p;
  long val = 0;

  if (fd < 0)
    return -1;
  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return -1;
  buf[n] = '\0';
  p = strstr(buf, "MemFree:");
  if (!p)
    return -1;
  while (*p && (*p < '0' || *p > '9'))
    p++;
  while (*p >= '0' && *p <= '9') {
    val = val * 10 + (*p - '0');
    p++;
  }
  return val;
}

int main(void) {
  long before = read_memfree_kb();
  unsigned char *p;
  long after_reserve;
  long after_touch;
  size_t offsets[8];

  if (before < 0) {
    printf("[FAIL] lazy_sparse_1g: meminfo before\n");
    return 1;
  }

  p = mmap(NULL, ONE_GIB, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    printf("[FAIL] lazy_sparse_1g: mmap 1GiB failed\n");
    return 1;
  }

  after_reserve = read_memfree_kb();
  if (after_reserve < 0) {
    printf("[FAIL] lazy_sparse_1g: meminfo after reserve\n");
    munmap(p, ONE_GIB);
    return 1;
  }
  if (before - after_reserve > 8 * (long)(PAGE_SIZE_BYTES / 1024)) {
    printf("[FAIL] lazy_sparse_1g: reserve used %ld KiB\n",
           before - after_reserve);
    munmap(p, ONE_GIB);
    return 1;
  }

  for (int i = 0; i < 8; i++)
    offsets[i] = ((ONE_GIB / PAGE_SIZE_BYTES - 1) * (size_t)i / 7) *
                 PAGE_SIZE_BYTES;
  for (int i = 0; i < 8; i++)
    p[offsets[i]] = (unsigned char)(0x80 + i);
  for (int i = 0; i < 8; i++) {
    if (p[offsets[i]] != (unsigned char)(0x80 + i)) {
      printf("[FAIL] lazy_sparse_1g: value mismatch %d\n", i);
      munmap(p, ONE_GIB);
      return 1;
    }
  }

  after_touch = read_memfree_kb();
  if (after_touch < 0) {
    printf("[FAIL] lazy_sparse_1g: meminfo after touch\n");
    munmap(p, ONE_GIB);
    return 1;
  }
  if (after_reserve - after_touch > 24 * (long)(PAGE_SIZE_BYTES / 1024)) {
    printf("[FAIL] lazy_sparse_1g: touch used %ld KiB\n",
           after_reserve - after_touch);
    munmap(p, ONE_GIB);
    return 1;
  }

  if (munmap(p, ONE_GIB) != 0) {
    printf("[FAIL] lazy_sparse_1g: munmap failed\n");
    return 1;
  }

  printf("[PASS] lazy_sparse_1g\n");
  return 0;
}
