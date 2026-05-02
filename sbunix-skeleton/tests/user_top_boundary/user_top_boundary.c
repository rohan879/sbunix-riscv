/*
 * user_top_boundary -- root entry 1 is user VA, KERNBASE remains excluded.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

#define PAGE_SIZE_BYTES 4096UL
#define OLD_TOP 0x40000000UL
#define USER_TOP 0x80000000UL
#define HIGH_USER 0x7E000000UL

int main(void) {
  unsigned char *mid = mmap((void *)OLD_TOP, PAGE_SIZE_BYTES,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (mid == MAP_FAILED) {
    printf("[FAIL] user_top_boundary: old top fixed map errno=%d\n", errno);
    return 1;
  }
  mid[0] = 0x5a;
  if (mid[0] != 0x5a) {
    printf("[FAIL] user_top_boundary: old top writeback\n");
    return 1;
  }
  munmap(mid, PAGE_SIZE_BYTES);

  unsigned char *hi = mmap((void *)HIGH_USER, PAGE_SIZE_BYTES,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (hi == MAP_FAILED) {
    printf("[FAIL] user_top_boundary: high user fixed map errno=%d\n", errno);
    return 1;
  }
  hi[PAGE_SIZE_BYTES - 1] = 0xa5;
  if (hi[PAGE_SIZE_BYTES - 1] != 0xa5) {
    printf("[FAIL] user_top_boundary: high user writeback\n");
    return 1;
  }
  munmap(hi, PAGE_SIZE_BYTES);

  errno = 0;
  void *bad = mmap((void *)USER_TOP, PAGE_SIZE_BYTES,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (bad != MAP_FAILED) {
    munmap(bad, PAGE_SIZE_BYTES);
    printf("[FAIL] user_top_boundary: KERNBASE mapped\n");
    return 1;
  }

  printf("[PASS] user_top_boundary\n");
  return 0;
}
