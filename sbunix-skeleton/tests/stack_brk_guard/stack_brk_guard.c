/*
 * stack_brk_guard -- stack growth keeps guard gaps against mmap and brk.
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <syscall.h>

#define PAGE 4096UL

static int wait_signaled(pid_t pid) {
  int status = 0;
  if (waitpid(pid, &status, 0) != pid)
    return 0;
  return WIFSIGNALED(status) ||
         (WIFEXITED(status) && WEXITSTATUS(status) == 101);
}

static int grow_and_fork_cow(void) {
  volatile char here;
  uintptr_t sp = (uintptr_t)&here;
  volatile char *target = (volatile char *)((sp - 32 * PAGE) & ~(PAGE - 1));
  int status = 0;
  pid_t pid;

  target[0] = 'p';
  if (target[0] != 'p')
    return 0;
  pid = fork();
  if (pid < 0)
    return 0;
  if (pid == 0) {
    target[0] = 'c';
    _exit(target[0] == 'c' ? 0 : 2);
  }
  if (waitpid(pid, &status, 0) != pid)
    return 0;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 && target[0] == 'p';
}

static int mmap_collision_rejected(void) {
  pid_t pid = fork();
  if (pid < 0)
    return 0;
  if (pid == 0) {
    volatile char here;
    uintptr_t sp = (uintptr_t)&here;
    char *hole = (char *)((sp - 32 * PAGE) & ~(PAGE - 1));
    volatile char *fault = (volatile char *)(hole + PAGE);
    void *m = mmap(hole, PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m == MAP_FAILED)
      _exit(3);
    *fault = 1;
    _exit(4);
  }
  return wait_signaled(pid);
}

static int brk_collision_rejected(void) {
  volatile char here;
  uintptr_t sp = (uintptr_t)&here;
  uintptr_t target = (sp - 32 * PAGE) & ~(PAGE - 1);
  uintptr_t before = (uintptr_t)sbrk(0);
  long raw;

  if (before == (uintptr_t)-1)
    return 0;
  raw = syscall(SYS_brk, target);
  return (uintptr_t)raw == before && (uintptr_t)sbrk(0) == before;
}

int main(void) {
  if (!mmap_collision_rejected()) {
    printf("[FAIL] stack_brk_guard: mmap collision\n");
    return 1;
  }
  if (!grow_and_fork_cow()) {
    printf("[FAIL] stack_brk_guard: grow fork private\n");
    return 1;
  }
  if (!brk_collision_rejected()) {
    printf("[FAIL] stack_brk_guard: brk collision\n");
    return 1;
  }
  printf("[PASS] stack_brk_guard\n");
  return 0;
}
