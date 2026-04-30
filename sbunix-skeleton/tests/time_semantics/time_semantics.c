#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/user_clock.h>
#include <sys/wait.h>
#include <syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef CLONE_VM
#define CLONE_VM    0x00000100
#endif
#ifndef CLONE_VFORK
#define CLONE_VFORK 0x00004000
#endif

static int g_pass;
static int g_fail;

#define CHECK(label, cond) do { \
  if (cond) { printf("[PASS] time_semantics: %s\n", label); g_pass++; } \
  else { printf("[FAIL] time_semantics: %s\n", label); g_fail++; } \
} while (0)

static long nsec_diff(const struct timespec *a, const struct timespec *b) {
  return (long)((b->tv_sec - a->tv_sec) * 1000000000L +
                (b->tv_nsec - a->tv_nsec));
}

static void burn_cpu(clock_t target_delta) {
  clock_t start = clock();
  volatile unsigned long sink = 0;

  while (clock() - start < target_delta) {
    for (int i = 0; i < 10000; i++)
      sink += (unsigned long)i;
  }
  (void)sink;
}

static int child_status(pid_t pid) {
  int status = 0;
  if (waitpid(pid, &status, 0) != pid)
    return -1;
  if (!WIFEXITED(status))
    return -1;
  return WEXITSTATUS(status);
}

static void test_nanosleep_short(void) {
  struct timespec a;
  struct timespec b;
  struct timespec req = {0, 50000000L};

  if (clock_gettime(CLOCK_MONOTONIC, &a) != 0) {
    CHECK("monotonic setup", 0);
    return;
  }
  CHECK("nanosleep 50ms returns success", nanosleep(&req, NULL) == 0);
  if (clock_gettime(CLOCK_MONOTONIC, &b) != 0) {
    CHECK("monotonic after sleep", 0);
    return;
  }
  long delta = nsec_diff(&a, &b);
  CHECK("nanosleep 50ms not early", delta >= 45000000L);
  CHECK("nanosleep 50ms not one-second rounded", delta < 200000000L);
}

static void test_clock_excludes_sleep(void) {
  clock_t before = clock();
  sleep(1);
  clock_t after_sleep = clock();
  burn_cpu(CLOCKS_PER_SEC / 20);
  clock_t after_busy = clock();

  CHECK("clock excludes one-second sleep",
        after_sleep - before < CLOCKS_PER_SEC / 4);
  CHECK("clock advances during CPU work",
        after_busy - after_sleep >= CLOCKS_PER_SEC / 25);
}

static void test_fork_clock_starts_fresh(void) {
  burn_cpu(CLOCKS_PER_SEC / 30);
  pid_t pid = fork();
  if (pid == 0) {
    clock_t child = clock();
    _exit(child < CLOCKS_PER_SEC / 4 ? 0 : 3);
  }
  CHECK("fork child CPU clock starts fresh", child_status(pid) == 0);
}

static void test_exec_clock_starts_fresh(const char *self) {
  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = {(char *)self, (char *)"--after-exec", NULL};
    char *envp[] = {NULL};
    burn_cpu(CLOCKS_PER_SEC / 20);
    execve(self, argv, envp);
    _exit(127);
  }
  CHECK("exec CPU clock starts fresh", child_status(pid) == 0);
}

static void test_vfork_clock_lifecycle(void) {
  long ret = syscall(SYS_clone, CLONE_VM | CLONE_VFORK | SIGCHLD,
                     0, 0, 0, 0);
  if (ret == 0) {
    clock_t c = clock();
    _exit(c >= 0 ? 0 : 4);
  }
  if (ret < 0) {
    CHECK("CLONE_VM vfork clock path", 0);
    return;
  }
  CHECK("CLONE_VM vfork clock path", child_status((pid_t)ret) == 0);
}

static void test_reserved_clock_page(void) {
  void *clock_va = (void *)USER_CLOCK_VA;
  void *sig_va = (void *)(USER_CLOCK_VA + 4096UL);
  long cur_brk = syscall(SYS_brk, 0);
  long wanted = (long)USER_CLOCK_VA + 4096L;

  errno = 0;
  void *p = mmap(clock_va, 4096, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  CHECK("mmap rejects CPU clock page", p == MAP_FAILED && errno == EINVAL);

  errno = 0;
  p = mmap(sig_va, 4096, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  CHECK("mmap rejects signal trampoline page", p == MAP_FAILED && errno == EINVAL);

  errno = 0;
  CHECK("mprotect rejects CPU clock page",
        mprotect(clock_va, 4096, PROT_READ) == -1 && errno == EINVAL);

  errno = 0;
  CHECK("munmap rejects CPU clock page",
        munmap(clock_va, 4096) == -1 && errno == EINVAL);

  errno = 0;
  CHECK("mremap rejects CPU clock page",
        mremap(clock_va, 4096, 8192, MREMAP_MAYMOVE) == MAP_FAILED &&
        errno == EINVAL);

  if (cur_brk > 0)
    CHECK("brk rejects CPU clock page", syscall(SYS_brk, wanted) == cur_brk);
  else
    CHECK("brk rejects CPU clock page", 0);
}

static void test_difftime_boundary(void) {
  struct timespec ts;
  time_t t0;
  time_t t1;

  for (;;) {
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
      CHECK("boundary realtime setup", 0);
      return;
    }
    if (ts.tv_nsec >= 900000000L)
      break;
    usleep(1000);
  }

  t0 = time(NULL);
  sleep(1);
  t1 = time(NULL);
  CHECK("time sleep difftime crosses one second",
        difftime(t1, t0) == 1.0);
}

int main(int argc, char **argv) {
  const char *self = (argc > 0 && argv[0]) ? argv[0] : "/test/bin/time_semantics";

  if (argc > 1 && strcmp(argv[1], "--after-exec") == 0)
    return clock() < CLOCKS_PER_SEC / 4 ? 0 : 2;

  test_nanosleep_short();
  test_clock_excludes_sleep();
  test_fork_clock_starts_fresh();
  test_exec_clock_starts_fresh(self);
  test_vfork_clock_lifecycle();
  test_reserved_clock_page();
  test_difftime_boundary();

  printf("=== time_semantics: %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
