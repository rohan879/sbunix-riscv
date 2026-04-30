/*
 * forkmemdiag -- Reproduce fork + memory-pressure crashes with size sweeps.
 *
 * Default mode ("cow"):
 *   The parent pre-faults an anonymous region, then forks. The child rewrites
 *   every page to force COW under pressure, verifies the data, and exits.
 *
 * Postfork mode ("postfork"):
 *   The parent forks first. The child then allocates and writes the region,
 *   which exercises the postfork allocation path.
 *
 * Default sweep: 30MB, 60MB
 * QEMU_MEM must remain 128M, so the default postfork sweep stays below the
 * available guest free memory after kernel and test image overhead.
 * Optional usage:
 *   forkmemdiag [cow] <mb> [<mb> ...]
 *   forkmemdiag postfork <mb> [<mb> ...]
 *   forkmemdiag postfork-recycle <repeats> <mb> [<mb> ...]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define PAGE_SIZE_BYTES 4096UL
#define DEFAULT_CASES 2
#define RECYCLE_DRIFT_LIMIT_KB 8192L

static long read_meminfo_kb(const char *key)
{
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

static int run_postfork_recycle_case_mb(size_t mb, long baseline)
{
  size_t len = mb * 1024UL * 1024UL;
  size_t npages = len / PAGE_SIZE_BYTES;
  long before = read_meminfo_kb("MemFree");

  if (before < 0) {
    printf("[FAIL] recycle: could not read MemFree before %lu MB\n",
           (unsigned long)mb);
    return 1;
  }

  printf("\n=== forkmemdiag postfork-recycle: %lu MB ===\n",
         (unsigned long)mb);

  pid_t pid = fork();
  if (pid < 0) {
    printf("[FAIL] recycle fork failed at %lu MB\n", (unsigned long)mb);
    return 1;
  }

  if (pid == 0) {
    unsigned char *region = mmap(NULL, len, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED)
      _exit(30);

    for (size_t i = 0; i < npages; i++) {
      unsigned char value = (unsigned char)(0x33 ^ (i & 0x7f));
      volatile unsigned char *p = &region[i * PAGE_SIZE_BYTES];
      *p = value;
      if (*p != value)
        _exit(31);
    }

    /* TEST: Leave the mapping live at _exit so kernel exit/reap cleanup,
     * not userspace munmap(), must recycle anonymous frames and page tables. */
    _exit(0);
  }

  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);
  if (waited != pid) {
    printf("[FAIL] recycle waitpid mismatch at %lu MB\n",
           (unsigned long)mb);
    return 1;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    if (WIFEXITED(status)) {
      printf("[FAIL] recycle child status %d at %lu MB\n",
             WEXITSTATUS(status), (unsigned long)mb);
    } else if (WIFSIGNALED(status)) {
      printf("[FAIL] recycle child signal %d at %lu MB\n",
             WTERMSIG(status), (unsigned long)mb);
    } else {
      printf("[FAIL] recycle child ended unexpectedly at %lu MB\n",
             (unsigned long)mb);
    }
    return 1;
  }

  long after = read_meminfo_kb("MemFree");
  if (after < 0) {
    printf("[FAIL] recycle: could not read MemFree after %lu MB\n",
           (unsigned long)mb);
    return 1;
  }

  long local_drop = before - after;
  long drift = baseline - after;
  if (local_drop < 0)
    local_drop = 0;
  if (drift < 0)
    drift = 0;

  if (local_drop > RECYCLE_DRIFT_LIMIT_KB ||
      drift > RECYCLE_DRIFT_LIMIT_KB) {
    printf("[FAIL] recycle %lu MB leaked: before=%ld after=%ld baseline=%ld\n",
           (unsigned long)mb, before, after, baseline);
    return 1;
  }

  printf("[PASS] recycle %lu MB: drop %ld KiB drift %ld KiB\n",
         (unsigned long)mb, local_drop, drift);
  return 0;
}

int main(int argc, char **argv)
{
  size_t defaults[DEFAULT_CASES] = {30, 60};
  int failures = 0;
  int first_fail = -1;
  int mode_postfork = 0;
  int mode_recycle = 0;
  int repeats = 1;
  int argi = 1;

  printf("forkmemdiag: fork + COW memory-pressure sweep\n");
  printf("hint: watch kernel output for refcount, rmap, or free warnings\n");

  if (argc > 1 && strcmp(argv[1], "postfork") == 0) {
    mode_postfork = 1;
    argi = 2;
    printf("mode: postfork allocation in child\n");
  } else if (argc > 1 && strcmp(argv[1], "postfork-recycle") == 0) {
    mode_recycle = 1;
    argi = 3;
    if (argc <= argi) {
      printf("[FAIL] postfork-recycle usage: repeats mb...\n");
      return 1;
    }
    repeats = atoi(argv[2]);
    if (repeats <= 0) {
      printf("[FAIL] invalid repeat count: %s\n", argv[2]);
      return 1;
    }
    printf("mode: postfork allocation with exit-path recycling\n");
  } else if (argc > 1 && strcmp(argv[1], "cow") == 0) {
    argi = 2;
    printf("mode: parent prefault + child COW rewrite\n");
  } else {
    printf("mode: parent prefault + child COW rewrite\n");
  }

  long recycle_baseline = mode_recycle ? read_meminfo_kb("MemFree") : 0;
  if (mode_recycle && recycle_baseline < 0) {
    printf("[FAIL] could not read recycle baseline MemFree\n");
    return 1;
  }

  if (argc > argi) {
    for (int i = argi; i < argc; i++) {
      int mb = atoi(argv[i]);
      if (mb <= 0) {
        printf("[FAIL] invalid size: %s\n", argv[i]);
        return 1;
      }
      for (int r = 0; r < repeats; r++) {
        int rc = mode_recycle
                   ? run_postfork_recycle_case_mb((size_t)mb, recycle_baseline)
                   : mode_postfork ? run_postfork_case_mb((size_t)mb)
                                   : run_case_mb((size_t)mb);
        if (rc != 0) {
          failures++;
          if (first_fail < 0)
            first_fail = mb;
        }
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
  printf("a resident-memory boundary or COW/exit cleanup regression.\n");
  return 1;
}
