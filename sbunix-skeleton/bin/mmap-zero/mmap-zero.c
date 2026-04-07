#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

struct probe_case {
  uint64_t addr;
  const char *label;
  int expect_ok;
};

static int verify_zero_page(unsigned char *p) {
  for (size_t i = 0; i < PAGE_SIZE; i++) {
    if (p[i] != 0) {
      dprintf(2, "  first non-zero byte at offset %lu: 0x%02x\n",
              (unsigned long)i, p[i]);
      return -1;
    }
  }
  return 0;
}

static int run_fixed_probe(const struct probe_case *pc) {
  void *ret = mmap((void *)pc->addr, PAGE_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (ret == MAP_FAILED) {
    dprintf(2, "  mmap failed: errno=%d\n", errno);
    return 100 + errno;
  }

  if ((uint64_t)ret != pc->addr) {
    dprintf(2, "  mmap returned wrong address: 0x%lx\n", (uint64_t)ret);
    return 2;
  }

  if (verify_zero_page((unsigned char *)ret) < 0)
    return 3;

  ((unsigned char *)ret)[0] = 0xa5;
  ((unsigned char *)ret)[PAGE_SIZE - 1] = 0x5a;
  if (((unsigned char *)ret)[0] != 0xa5 ||
      ((unsigned char *)ret)[PAGE_SIZE - 1] != 0x5a) {
    dprintf(2, "  write/readback failed\n");
    return 4;
  }

  if (munmap(ret, PAGE_SIZE) < 0) {
    dprintf(2, "  munmap failed: errno=%d\n", errno);
    return 5;
  }

  return 0;
}

static int report_child_result(const struct probe_case *pc, int status) {
  if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
    printf("fixed 0x%08lx %-26s -> signal %d\n",
           (unsigned long)pc->addr, pc->label, sig);
    return 1;
  }

  if (!WIFEXITED(status)) {
    printf("fixed 0x%08lx %-26s -> unknown status 0x%x\n",
           (unsigned long)pc->addr, pc->label, status);
    return 1;
  }

  {
    int code = WEXITSTATUS(status);
    if (code >= 100) {
      int err = code - 100;
      printf("fixed 0x%08lx %-26s -> mmap errno %d%s\n",
             (unsigned long)pc->addr, pc->label, err,
             (err == EINVAL) ? " (EINVAL)" : "");
      return pc->expect_ok ? 1 : (err == EINVAL ? 0 : 1);
    }

    printf("fixed 0x%08lx %-26s -> ok\n",
           (unsigned long)pc->addr, pc->label);
    return pc->expect_ok ? 0 : 1;
  }
}

int main(void) {
  static const struct probe_case cases[] = {
    {0x00400000UL, "low free region", 1},
    {0x0c000000UL, "PLIC MMIO", 0},
    {0x10000000UL, "UART MMIO", 0},
    {0x10200000UL, "after UART slot", 1},
    {0x20000000UL, "old mmap base", 1},
    {0x2fe00000UL, "below PCIe ECAM", 1},
    {0x30000000UL, "PCIe ECAM", 0},
    {0x3fe00000UL, "high free region", 1},
    {0x3fd00000UL, "high PCIe ECAM", 0},
  };
  int failed = 0;

  {
    void *ret = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ret == MAP_FAILED) {
      printf("auto mmap                    -> errno %d\n", errno);
      failed = 1;
    } else {
      int zero_ok = verify_zero_page((unsigned char *)ret) == 0;
      printf("auto mmap                    -> 0x%08lx %s\n",
             (unsigned long)(uintptr_t)ret, zero_ok ? "zeroed" : "non-zero");
      if (!zero_ok)
        failed = 1;
      munmap(ret, PAGE_SIZE);
    }
  }

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    pid_t pid = fork();
    int status = 0;

    if (pid < 0) {
      perror("fork");
      return 1;
    }
    if (pid == 0)
      _exit(run_fixed_probe(&cases[i]));
    if (waitpid(pid, &status, 0) < 0) {
      perror("waitpid");
      return 1;
    }
    if (report_child_result(&cases[i], status) != 0)
      failed = 1;
  }

  return failed ? 1 : 0;
}
