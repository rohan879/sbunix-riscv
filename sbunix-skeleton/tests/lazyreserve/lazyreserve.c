/*
 * lazyreserve -- large brk/mmap reservations must not allocate eagerly.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define PAGE_SIZE_BYTES 4096UL
#define RESERVE_MB 64UL
#define TOUCHES 16

static long read_meminfo_kb(const char *key) {
  int fd = open("/proc/meminfo", O_RDONLY);
  if (fd < 0)
    return -1;

  char buf[2048];
  int n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return -1;
  buf[n] = '\0';

  char *p = strstr(buf, key);
  if (!p)
    return -1;
  p += strlen(key);
  while (*p == ':' || *p == ' ' || *p == '\t')
    p++;

  long val = 0;
  while (*p >= '0' && *p <= '9') {
    val = val * 10 + (*p - '0');
    p++;
  }
  return val;
}

static int touch_sparse(unsigned char *p, size_t len, unsigned char seed) {
  size_t stride = len / TOUCHES;
  stride = (stride / PAGE_SIZE_BYTES) * PAGE_SIZE_BYTES;
  if (stride == 0)
    stride = PAGE_SIZE_BYTES;

  for (int i = 0; i < TOUCHES; i++) {
    size_t off = (size_t)i * stride;
    if (off >= len)
      off = len - PAGE_SIZE_BYTES;
    p[off] = (unsigned char)(seed + i);
  }

  for (int i = 0; i < TOUCHES; i++) {
    size_t off = (size_t)i * stride;
    if (off >= len)
      off = len - PAGE_SIZE_BYTES;
    if (p[off] != (unsigned char)(seed + i))
      return 0;
  }
  return 1;
}

int main(void) {
  size_t len = RESERVE_MB * 1024UL * 1024UL;
  long free_before = read_meminfo_kb("MemFree");
  if (free_before < 0) {
    printf("[FAIL] lazyreserve: could not read MemFree before\n");
    return 1;
  }

  unsigned char *map = mmap(NULL, len, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (map == MAP_FAILED) {
    printf("[FAIL] lazyreserve: mmap reserve failed\n");
    return 1;
  }

  unsigned char *old_brk = sbrk(0);
  unsigned char *heap = sbrk((long)len);
  if (old_brk == (void *)-1 || heap == (void *)-1 || heap != old_brk) {
    printf("[FAIL] lazyreserve: sbrk reserve failed\n");
    munmap(map, len);
    return 1;
  }

  long free_after_reserve = read_meminfo_kb("MemFree");
  if (free_after_reserve < 0) {
    printf("[FAIL] lazyreserve: could not read MemFree after reserve\n");
    munmap(map, len);
    sbrk(-(long)len);
    return 1;
  }

  long reserve_drop = free_before - free_after_reserve;
  if (reserve_drop < 0)
    reserve_drop = 0;
  if (reserve_drop > 512) {
    printf("[FAIL] lazyreserve: reservation consumed %ld KiB\n", reserve_drop);
    munmap(map, len);
    sbrk(-(long)len);
    return 1;
  }

  if (!touch_sparse(map, len, 0x40) || !touch_sparse(heap, len, 0x70)) {
    printf("[FAIL] lazyreserve: sparse touches did not verify\n");
    munmap(map, len);
    sbrk(-(long)len);
    return 1;
  }

  munmap(map, len);
  sbrk(-(long)len);
  printf("[PASS] lazyreserve: reserve drop %ld KiB, sparse touches ok\n",
         reserve_drop);
  return 0;
}
