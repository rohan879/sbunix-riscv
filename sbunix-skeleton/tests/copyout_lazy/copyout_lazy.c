#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096

int main(void) {
  int pfd[2];
  const char msg[] = "kernel copyout into a never-touched mmap page";

  printf("=== copyout_lazy ===\n");

  char *dst = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (dst == MAP_FAILED) {
    printf("[FAIL] copyout_lazy: mmap failed\n");
    return 1;
  }

  if (pipe(pfd) < 0) {
    printf("[FAIL] copyout_lazy: pipe failed\n");
    return 1;
  }

  if (write(pfd[1], msg, sizeof(msg)) != (int)sizeof(msg)) {
    printf("[FAIL] copyout_lazy: pipe write failed\n");
    return 1;
  }

  int n = read(pfd[0], dst, sizeof(msg));
  if (n != (int)sizeof(msg) || strcmp(dst, msg) != 0) {
    printf("[FAIL] copyout_lazy: read into lazy mapping failed\n");
    return 1;
  }

  munmap(dst, PAGE);
  close(pfd[0]);
  close(pfd[1]);
  printf("[PASS] copyout_lazy\n");
  return 0;
}
