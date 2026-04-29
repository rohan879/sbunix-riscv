/*
 * cow_mprotect_fault -- protect COW pages against direct and kernel writes.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define PAGE_SIZE_BYTES 4096UL
#define SEGV_SENTINEL 77

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(label, cond) do { \
  if (cond) { \
    printf("[PASS] cow_mprotect_fault: %s\n", label); \
    g_pass++; \
  } else { \
    printf("[FAIL] cow_mprotect_fault: %s\n", label); \
    g_fail++; \
  } \
} while (0)

static void segv_handler(int sig) {
  (void)sig;
  _exit(SEGV_SENTINEL);
}

static void install_segv_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = segv_handler;
  sigaction(SIGSEGV, &sa, 0);
}

static int wait_for_exit(pid_t pid, int want) {
  int status = 0;
  if (waitpid(pid, &status, 0) != pid)
    return 0;
  return WIFEXITED(status) && WEXITSTATUS(status) == want;
}

static void test_direct_write_segv(void) {
  char *p = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("direct SIGSEGV sentinel", 0);
    return;
  }

  memset(p, 0x5a, PAGE_SIZE_BYTES);
  pid_t pid = fork();
  if (pid < 0) {
    munmap(p, PAGE_SIZE_BYTES);
    CHECK("direct SIGSEGV sentinel", 0);
    return;
  }

  if (pid == 0) {
    install_segv_handler();
    if (mprotect(p, PAGE_SIZE_BYTES, PROT_READ) != 0)
      _exit(10);
    p[0] = 0x11;
    _exit(11);
  }

  CHECK("direct SIGSEGV sentinel", wait_for_exit(pid, SEGV_SENTINEL));
  munmap(p, PAGE_SIZE_BYTES);
}

static void test_copyout_efault_unchanged(void) {
  int pfd[2];
  char *p = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    CHECK("copyout EFAULT unchanged", 0);
    return;
  }

  memset(p, 0x33, PAGE_SIZE_BYTES);
  p[0] = 'A';
  p[1] = 'B';

  if (pipe(pfd) != 0) {
    munmap(p, PAGE_SIZE_BYTES);
    CHECK("copyout EFAULT unchanged", 0);
    return;
  }

  if (write(pfd[1], "Z", 1) != 1) {
    close(pfd[0]);
    close(pfd[1]);
    munmap(p, PAGE_SIZE_BYTES);
    CHECK("copyout EFAULT unchanged", 0);
    return;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pfd[0]);
    close(pfd[1]);
    munmap(p, PAGE_SIZE_BYTES);
    CHECK("copyout EFAULT unchanged", 0);
    return;
  }

  if (pid == 0) {
    close(pfd[1]);
    if (mprotect(p, PAGE_SIZE_BYTES, PROT_READ) != 0)
      _exit(20);
    errno = 0;
    ssize_t n = read(pfd[0], p, 1);
    if (!(n == -1 && errno == EFAULT))
      _exit(21);
    if (p[0] != 'A' || p[1] != 'B')
      _exit(22);
    _exit(0);
  }

  close(pfd[0]);
  close(pfd[1]);
  CHECK("copyout EFAULT unchanged", wait_for_exit(pid, 0));
  munmap(p, PAGE_SIZE_BYTES);
}

int main(void) {
  alarm(30);
  test_direct_write_segv();
  test_copyout_efault_unchanged();

  if (g_fail == 0) {
    printf("[PASS] cow_mprotect_fault\n");
    return 0;
  }

  printf("[FAIL] cow_mprotect_fault: %d failed, %d passed\n", g_fail, g_pass);
  return 1;
}
