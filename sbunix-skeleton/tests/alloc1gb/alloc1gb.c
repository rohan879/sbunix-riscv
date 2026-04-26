/*
 * alloc1gb -- probe large virtual reservations without requiring 1 GiB RAM.
 *
 * The test keeps physical pressure sparse while checking VMA coverage,
 * inherited fork data, private COW writes, munmap cleanup, and post-unmap
 * fault behavior.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define PAGE_SIZE_BYTES 4096UL

static unsigned long parse_hex(const char **pp)
{
  const char *p = *pp;
  unsigned long value = 0;

  while ((*p >= '0' && *p <= '9') ||
         (*p >= 'a' && *p <= 'f') ||
         (*p >= 'A' && *p <= 'F')) {
    int digit;
    if (*p >= '0' && *p <= '9')
      digit = *p - '0';
    else if (*p >= 'a' && *p <= 'f')
      digit = *p - 'a' + 10;
    else
      digit = *p - 'A' + 10;
    value = (value << 4) | (unsigned long)digit;
    p++;
  }

  *pp = p;
  return value;
}

static int maps_cover(void *addr, size_t len)
{
  FILE *f = fopen("/proc/self/maps", "r");
  char line[128];
  unsigned long want_start = (unsigned long)addr;
  unsigned long want_end = want_start + (unsigned long)len;

  if (!f)
    return 0;

  while (fgets(line, sizeof(line), f)) {
    const char *p = line;
    unsigned long start = parse_hex(&p);
    if (*p != '-')
      continue;
    p++;
    unsigned long end = parse_hex(&p);
    if (start <= want_start && end >= want_end) {
      fclose(f);
      return 1;
    }
  }

  fclose(f);
  return 0;
}

static size_t sample_offset(size_t len, int idx)
{
  size_t page_count = len / PAGE_SIZE_BYTES;
  size_t page;

  switch (idx) {
  case 0: page = 0; break;
  case 1: page = page_count / 4; break;
  case 2: page = page_count / 2; break;
  case 3: page = (page_count * 3) / 4; break;
  default: page = page_count - 1; break;
  }

  if (page >= page_count)
    page = page_count - 1;
  return page * PAGE_SIZE_BYTES;
}

static int post_unmap_access_dies(volatile unsigned char *p)
{
  pid_t pid = fork();
  int status = 0;

  if (pid < 0)
    return 0;
  if (pid == 0) {
    volatile unsigned char value = p[0];
    (void)value;
    _exit(77);
  }

  if (waitpid(pid, &status, 0) != pid)
    return 0;
  if (WIFSIGNALED(status))
    return WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS;
  return !WIFEXITED(status) || WEXITSTATUS(status) != 77;
}

static int run_sparse_case(size_t mb)
{
  size_t len = mb * 1024UL * 1024UL;
  volatile unsigned char *region;
  unsigned char parent_values[5];
  pid_t pid;
  int status = 0;

  errno = 0;
  region = mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED)
    return errno == ENOMEM ? 2 : -1;

  if (!maps_cover((void *)region, len)) {
    munmap((void *)region, len);
    return -2;
  }

  for (int i = 0; i < 5; i++) {
    size_t off = sample_offset(len, i);
    parent_values[i] = (unsigned char)(0x31 + i);
    region[off] = parent_values[i];
    if (region[off] != parent_values[i]) {
      munmap((void *)region, len);
      return -3;
    }
  }

  pid = fork();
  if (pid < 0) {
    munmap((void *)region, len);
    return -4;
  }

  if (pid == 0) {
    for (int i = 0; i < 5; i++) {
      size_t off = sample_offset(len, i);
      if (region[off] != (unsigned char)(0x31 + i))
        _exit(20 + i);
      region[off] = (unsigned char)(0xa0 + i);
      if (region[off] != (unsigned char)(0xa0 + i))
        _exit(40 + i);
    }
    _exit(0);
  }

  if (waitpid(pid, &status, 0) != pid) {
    munmap((void *)region, len);
    return -5;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    munmap((void *)region, len);
    return -6;
  }

  for (int i = 0; i < 5; i++) {
    size_t off = sample_offset(len, i);
    if (region[off] != parent_values[i]) {
      munmap((void *)region, len);
      return -7;
    }
  }

  if (munmap((void *)region, len) != 0)
    return -8;

  if (!post_unmap_access_dies(region))
    return -9;

  return 0;
}

int main(void)
{
  static const size_t fallbacks[] = {768, 512, 384, 256, 128};
  int rc = run_sparse_case(1024);

  if (rc == 0) {
    printf("[PASS] alloc1gb: 1GiB sparse/fork/cleanup checks passed\n");
    return 0;
  }

  if (rc != 2) {
    printf("[FAIL] alloc1gb: 1GiB probe failed rc=%d errno=%d\n", rc, errno);
    return 1;
  }

  for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
    rc = run_sparse_case(fallbacks[i]);
    if (rc == 0) {
      printf("[PASS] alloc1gb: 1GiB rejected cleanly; %luMiB fallback passed\n",
             (unsigned long)fallbacks[i]);
      return 0;
    }
    if (rc != 2) {
      printf("[FAIL] alloc1gb: %luMiB fallback failed rc=%d errno=%d\n",
             (unsigned long)fallbacks[i], rc, errno);
      return 1;
    }
  }

  printf("[FAIL] alloc1gb: no sparse fallback reservation fit\n");
  return 1;
}
