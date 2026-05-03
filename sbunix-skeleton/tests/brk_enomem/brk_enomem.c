/* brk_enomem -- verify rejected brk growth leaves the break unchanged.
 *
 * BF: Large or colliding brk requests must not partially update process
 * size or VMA metadata.  The raw SYS_brk convention asserted here is:
 * success returns the requested break, rejection returns the current break. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <syscall.h>

#define PAGE_SIZE_BYTES 4096UL
#define ONE_GIB (1024UL * 1024UL * 1024UL)
#define USER_TOP_ADDR 0x80000000UL
#define SIGTRAMP_ADDR 0x7F000000UL
#define KERNEL_ADDR 0x80000000UL

static uintptr_t current_break(void)
{
  return (uintptr_t)sbrk(0);
}

static int expect_raw_reject(uintptr_t target, const char *label)
{
  uintptr_t before = current_break();
  long raw;
  uintptr_t after;

  if (before == (uintptr_t)-1) {
    printf("[FAIL] brk_enomem (%s: sbrk(0) failed before check)\n", label);
    return 1;
  }

  raw = syscall(SYS_brk, target);
  after = current_break();

  if ((uintptr_t)raw != before || after != before) {
    syscall(SYS_brk, before);
    printf("[FAIL] brk_enomem (%s: raw=0x%lx before=0x%lx after=0x%lx)\n",
           label, (unsigned long)raw, (unsigned long)before,
           (unsigned long)after);
    return 1;
  }

  return 0;
}

static int expect_sbrk_reject(long increment, const char *label)
{
  uintptr_t before = current_break();
  void *ret;
  uintptr_t after;

  if (before == (uintptr_t)-1) {
    printf("[FAIL] brk_enomem (%s: sbrk(0) failed before check)\n", label);
    return 1;
  }

  errno = 0;
  ret = sbrk(increment);
  after = current_break();

  if (ret != (void *)-1 || after != before) {
    syscall(SYS_brk, before);
    printf("[FAIL] brk_enomem (%s: ret=%p before=0x%lx after=0x%lx)\n",
           label, ret, (unsigned long)before, (unsigned long)after);
    return 1;
  }

  return 0;
}

static int child_status_ok(pid_t pid, int status, const char *label)
{
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    return 0;
  if (WIFEXITED(status)) {
    printf("[FAIL] brk_enomem (%s: child exit %d)\n",
           label, WEXITSTATUS(status));
    return 1;
  }
  if (WIFSIGNALED(status)) {
    printf("[FAIL] brk_enomem (%s: child signal %d)\n",
           label, WTERMSIG(status));
    return 1;
  }
  printf("[FAIL] brk_enomem (%s: child status 0x%x pid %d)\n",
         label, status, pid);
  return 1;
}

static int run_raw_child(uintptr_t target, const char *label)
{
  int status = 0;
  pid_t pid = fork();
  if (pid < 0) {
    printf("[FAIL] brk_enomem (%s: fork failed)\n", label);
    return 1;
  }
  if (pid == 0)
    _exit(expect_raw_reject(target, label) == 0 ? 0 : 1);
  if (waitpid(pid, &status, 0) != pid) {
    printf("[FAIL] brk_enomem (%s: waitpid failed)\n", label);
    return 1;
  }
  return child_status_ok(pid, status, label);
}

static int run_sbrk_child(long increment, const char *label)
{
  int status = 0;
  pid_t pid = fork();
  if (pid < 0) {
    printf("[FAIL] brk_enomem (%s: fork failed)\n", label);
    return 1;
  }
  if (pid == 0)
    _exit(expect_sbrk_reject(increment, label) == 0 ? 0 : 1);
  if (waitpid(pid, &status, 0) != pid) {
    printf("[FAIL] brk_enomem (%s: waitpid failed)\n", label);
    return 1;
  }
  return child_status_ok(pid, status, label);
}

static int run_mmap_collision_child(const char *label)
{
  int status = 0;
  pid_t pid = fork();
  if (pid < 0) {
    printf("[FAIL] brk_enomem (%s: fork failed)\n", label);
    return 1;
  }
  if (pid == 0) {
    void *mapped = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED)
      _exit(2);
    int rc = expect_raw_reject((uintptr_t)mapped + PAGE_SIZE_BYTES, label);
    munmap(mapped, PAGE_SIZE_BYTES);
    _exit(rc == 0 ? 0 : 1);
  }
  if (waitpid(pid, &status, 0) != pid) {
    printf("[FAIL] brk_enomem (%s: waitpid failed)\n", label);
    return 1;
  }
  return child_status_ok(pid, status, label);
}

static int run_repeated_raw_child(uintptr_t target, const char *label)
{
  int status = 0;
  pid_t pid = fork();
  if (pid < 0) {
    printf("[FAIL] brk_enomem (%s: fork failed)\n", label);
    return 1;
  }
  if (pid == 0) {
    for (int i = 0; i < 3; i++) {
      if (expect_raw_reject(target, label) != 0)
        _exit(1);
    }
    _exit(0);
  }
  if (waitpid(pid, &status, 0) != pid) {
    printf("[FAIL] brk_enomem (%s: waitpid failed)\n", label);
    return 1;
  }
  return child_status_ok(pid, status, label);
}

int main(void) {
  uintptr_t cur = (uintptr_t)sbrk(0);
  if (cur == (uintptr_t)-1) {
    printf("[FAIL] brk_enomem (sbrk(0) failed)\n");
    return 1;
  }

  uintptr_t small = cur + 4096;
  void *ret = sbrk(4096);
  if (ret == (void *)-1) {
    printf("[FAIL] brk_enomem (small sbrk failed)\n");
    return 1;
  }

  uintptr_t after = (uintptr_t)sbrk(0);
  if (after < small) {
    printf("[FAIL] brk_enomem (brk did not advance after small sbrk)\n");
    return 1;
  }

  if (run_raw_child(after + 2 * ONE_GIB, "raw oversized growth") != 0)
    return 1;

  if (run_sbrk_child((long)(2 * ONE_GIB), "sbrk oversized growth") != 0)
    return 1;

  if (run_sbrk_child(-(long)(2 * PAGE_SIZE_BYTES),
                     "sbrk below initial break") != 0)
    return 1;

  if (run_raw_child(USER_TOP_ADDR, "user top collision") != 0)
    return 1;
  if (run_raw_child(SIGTRAMP_ADDR, "signal trampoline collision") != 0)
    return 1;
  if (run_raw_child(KERNEL_ADDR, "kernel address") != 0)
    return 1;
  if (run_raw_child(UINTPTR_MAX - 2048UL, "rounding overflow") != 0)
    return 1;

  if (run_mmap_collision_child("mmap collision") != 0)
    return 1;

  if (run_repeated_raw_child(after + 2 * ONE_GIB,
                             "repeated oversized growth") != 0)
    return 1;

  printf("[PASS] brk_enomem\n");
  return 0;
}
