/*
 * forkmemdiag -- Reproduce fork + memory-pressure crashes with size sweeps.
 *
 * Default mode ("cow"):
 *   The parent pre-faults an anonymous region, then forks. The child rewrites
 *   every page to force COW under pressure, verifies the data, and exits.
 *
 * Postfork mode ("postfork"):
 *   The parent forks first. The child then allocates and writes the region,
 *   which more closely matches a "child allocates 100MB" grading scenario.
 *
 * Default sweep: 30MB, 60MB, 100MB
 * Optional usage:
 *   forkmemdiag [cow] <mb> [<mb> ...]
 *   forkmemdiag postfork <mb> [<mb> ...]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define PAGE_SIZE_BYTES 4096UL
#define DEFAULT_CASES 3

static int run_case_mb(size_t mb)
{
  size_t len = mb * 1024UL * 1024UL;
  size_t npages = len / PAGE_SIZE_BYTES;
  unsigned char *region;
  size_t i;

  printf("\n=== forkmemdiag: %lu MB ===\n", (unsigned long)mb);

  region = mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED) {
    printf("[FAIL] mmap %lu MB failed before fork\n", (unsigned long)mb);
    return 1;
  }

  for (i = 0; i < npages; i++) {
    region[i * PAGE_SIZE_BYTES] = (unsigned char)(i & 0x7f);
  }

  pid_t pid = fork();
  if (pid < 0) {
    printf("[FAIL] fork failed at %lu MB\n", (unsigned long)mb);
    munmap(region, len);
    return 1;
  }

  if (pid == 0) {
    for (i = 0; i < npages; i++) {
      unsigned char value = (unsigned char)(0xa5 ^ (i & 0x3f));
      volatile unsigned char *p = &region[i * PAGE_SIZE_BYTES];
      *p = value;
      if (*p != value)
        _exit(10);

      if ((i & 1023UL) == 0) {
        size_t touched_mb = ((i + 1) * PAGE_SIZE_BYTES) / (1024UL * 1024UL);
        if (touched_mb > 0 && (touched_mb % 10UL) == 0) {
          printf("child: touched %lu MB\n", (unsigned long)touched_mb);
        }
      }
    }

    for (i = 0; i < npages; i++) {
      unsigned char want = (unsigned char)(0xa5 ^ (i & 0x3f));
      if (region[i * PAGE_SIZE_BYTES] != want)
        _exit(11);
    }

    _exit(0);
  }

  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);
  munmap(region, len);

  if (waited != pid) {
    printf("[FAIL] waitpid mismatch at %lu MB\n", (unsigned long)mb);
    return 1;
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    printf("[PASS] child exited cleanly at %lu MB\n", (unsigned long)mb);
    return 0;
  }
  if (WIFEXITED(status)) {
    printf("[FAIL] child exited with status %d at %lu MB\n",
           WEXITSTATUS(status), (unsigned long)mb);
    return 1;
  }
  if (WIFSIGNALED(status)) {
    printf("[FAIL] child died from signal %d at %lu MB\n",
           WTERMSIG(status), (unsigned long)mb);
    return 1;
  }

  printf("[FAIL] child ended unexpectedly at %lu MB (status=0x%x)\n",
         (unsigned long)mb, status);
  return 1;
}

static int run_postfork_case_mb(size_t mb)
{
  size_t len = mb * 1024UL * 1024UL;
  size_t npages = len / PAGE_SIZE_BYTES;

  printf("\n=== forkmemdiag postfork: %lu MB ===\n", (unsigned long)mb);

  pid_t pid = fork();
  if (pid < 0) {
    printf("[FAIL] fork failed at %lu MB\n", (unsigned long)mb);
    return 1;
  }

  if (pid == 0) {
    unsigned char *region = mmap(NULL, len, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED)
      _exit(20);

    for (size_t i = 0; i < npages; i++) {
      unsigned char value = (unsigned char)(0x5a ^ (i & 0x3f));
      volatile unsigned char *p = &region[i * PAGE_SIZE_BYTES];
      *p = value;
      if (*p != value)
        _exit(21);

      if ((i & 1023UL) == 0) {
        size_t touched_mb = ((i + 1) * PAGE_SIZE_BYTES) / (1024UL * 1024UL);
        if (touched_mb > 0 && (touched_mb % 10UL) == 0) {
          printf("child(postfork): touched %lu MB\n", (unsigned long)touched_mb);
        }
      }
    }

    for (size_t i = 0; i < npages; i++) {
      unsigned char want = (unsigned char)(0x5a ^ (i & 0x3f));
      if (region[i * PAGE_SIZE_BYTES] != want)
        _exit(22);
    }

    munmap(region, len);
    _exit(0);
  }

  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);
  if (waited != pid) {
    printf("[FAIL] waitpid mismatch at %lu MB\n", (unsigned long)mb);
    return 1;
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    printf("[PASS] postfork child exited cleanly at %lu MB\n", (unsigned long)mb);
    return 0;
  }
  if (WIFEXITED(status)) {
    printf("[FAIL] postfork child exited with status %d at %lu MB\n",
           WEXITSTATUS(status), (unsigned long)mb);
    return 1;
  }
  if (WIFSIGNALED(status)) {
    printf("[FAIL] postfork child died from signal %d at %lu MB\n",
           WTERMSIG(status), (unsigned long)mb);
    return 1;
  }

  printf("[FAIL] postfork child ended unexpectedly at %lu MB (status=0x%x)\n",
         (unsigned long)mb, status);
  return 1;
}

int main(int argc, char **argv)
{
  size_t defaults[DEFAULT_CASES] = {30, 60, 100};
  int failures = 0;
  int first_fail = -1;
  int mode_postfork = 0;
  int argi = 1;

  printf("forkmemdiag: fork + COW memory-pressure sweep\n");
  printf("hint: watch kernel output for 'WARNING: double kfree'\n");

  if (argc > 1 && strcmp(argv[1], "postfork") == 0) {
    mode_postfork = 1;
    argi = 2;
    printf("mode: postfork allocation in child\n");
  } else if (argc > 1 && strcmp(argv[1], "cow") == 0) {
    argi = 2;
    printf("mode: parent prefault + child COW rewrite\n");
  } else {
    printf("mode: parent prefault + child COW rewrite\n");
  }

  if (argc > argi) {
    for (int i = argi; i < argc; i++) {
      int mb = atoi(argv[i]);
      if (mb <= 0) {
        printf("[FAIL] invalid size: %s\n", argv[i]);
        return 1;
      }
      int rc = mode_postfork ? run_postfork_case_mb((size_t)mb)
                             : run_case_mb((size_t)mb);
      if (rc != 0) {
        failures++;
        if (first_fail < 0)
          first_fail = mb;
      }
    }
  } else {
    for (int i = 0; i < DEFAULT_CASES; i++) {
      int rc = mode_postfork ? run_postfork_case_mb(defaults[i])
                             : run_case_mb(defaults[i]);
      if (rc != 0) {
        failures++;
        if (first_fail < 0)
          first_fail = (int)defaults[i];
      }
    }
  }

  if (failures == 0) {
    printf("\nforkmemdiag: all sizes passed\n");
    return 0;
  }

  printf("\nforkmemdiag: first failing size = %d MB\n", first_fail);
  printf("diagnosis: if lower sizes pass and a higher size fails, the bug is likely\n");
  printf("triggered when reclaim/swap starts overlapping with COW or exit cleanup.\n");
  return 1;
}
