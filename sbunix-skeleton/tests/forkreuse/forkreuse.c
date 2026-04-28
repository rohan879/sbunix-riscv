/*
 * forkreuse -- verify exited children release large anonymous mappings.
 *
 * Each child reserves and faults a 30 MiB private mapping, verifies one byte
 * per page, and exits without munmap().  The parent repeats this enough times
 * to catch address-space teardown leaks and stale refcount ownership.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define PAGE_SIZE_BYTES 4096UL
#define CHILDREN 12
#define MAP_MB 30UL

static int child_work(int round) {
  size_t len = MAP_MB * 1024UL * 1024UL;
  size_t npages = len / PAGE_SIZE_BYTES;
  unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    return 10;

  for (size_t i = 0; i < npages; i++) {
    unsigned char value = (unsigned char)(0x31 + ((round + (int)i) & 0x3f));
    p[i * PAGE_SIZE_BYTES] = value;
  }

  for (size_t i = 0; i < npages; i++) {
    unsigned char want = (unsigned char)(0x31 + ((round + (int)i) & 0x3f));
    if (p[i * PAGE_SIZE_BYTES] != want)
      return 11;
  }

  return 0;
}

int main(void) {
  printf("forkreuse: %d children, %lu MiB each\n", CHILDREN, MAP_MB);
  alarm(120);

  for (int round = 0; round < CHILDREN; round++) {
    pid_t pid = fork();
    if (pid < 0) {
      printf("[FAIL] forkreuse: fork failed at round %d\n", round);
      return 1;
    }
    if (pid == 0)
      _exit(child_work(round));

    int status = 0;
    pid_t waited = waitpid(pid, &status, 0);
    if (waited != pid) {
      printf("[FAIL] forkreuse: waitpid mismatch at round %d\n", round);
      return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      printf("[FAIL] forkreuse: child round %d status=0x%x\n",
             round, status);
      return 1;
    }
  }

  printf("[PASS] forkreuse: all children completed\n");
  return 0;
}
