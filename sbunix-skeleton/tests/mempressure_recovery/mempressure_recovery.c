/*
 * mempressure_recovery -- low-memory exit and SIGKILL recycling coverage.
 *
 * TEST: Under QEMU_MEM=128M, repeated children fault large anonymous
 * mappings and then rely on process teardown, not munmap(), to return pages.
 * Alternating normal exit with SIGKILL catches cleanup paths that publish a
 * zombie before the dying process has released its user address space.
 */

#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define PAGE_SIZE_BYTES 4096UL
#define PRESSURE_MB 40UL
#define CYCLES 2

static int touch_pressure_mapping(int seed)
{
  size_t len = PRESSURE_MB * 1024UL * 1024UL;
  size_t npages = len / PAGE_SIZE_BYTES;
  unsigned char *region = mmap(NULL, len, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED)
    return 10;

  for (size_t i = 0; i < npages; i++) {
    unsigned char value = (unsigned char)(0x41 + ((seed + (int)i) & 0x1f));
    volatile unsigned char *p = &region[i * PAGE_SIZE_BYTES];
    *p = value;
  }

  for (size_t i = 0; i < npages; i++) {
    unsigned char want = (unsigned char)(0x41 + ((seed + (int)i) & 0x1f));
    if (region[i * PAGE_SIZE_BYTES] != want)
      return 11;
  }

  return 0;
}

static int expect_clean_exit(pid_t pid, int status, const char *label)
{
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    return 0;

  if (WIFEXITED(status)) {
    printf("[FAIL] mempressure_recovery: %s exited %d\n",
           label, WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    printf("[FAIL] mempressure_recovery: %s signaled %d\n",
           label, WTERMSIG(status));
  } else {
    printf("[FAIL] mempressure_recovery: %s status=0x%x pid=%d\n",
           label, status, pid);
  }
  return 1;
}

static int run_exit_child(int round)
{
  int status = 0;
  pid_t pid = fork();
  if (pid < 0) {
    printf("[FAIL] mempressure_recovery: fork failed in exit round %d\n", round);
    return 1;
  }

  if (pid == 0)
    _exit(touch_pressure_mapping(round));

  if (waitpid(pid, &status, 0) != pid) {
    printf("[FAIL] mempressure_recovery: waitpid failed in exit round %d\n",
           round);
    return 1;
  }

  if (expect_clean_exit(pid, status, "exit child") != 0)
    return 1;

  printf("[PASS] mempressure_recovery: exit round %d recycled pages\n", round);
  return 0;
}

static int run_kill_child(int round)
{
  int ready[2];
  int status = 0;

  if (pipe(ready) != 0) {
    printf("[FAIL] mempressure_recovery: pipe failed in kill round %d\n", round);
    return 1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(ready[0]);
    close(ready[1]);
    printf("[FAIL] mempressure_recovery: fork failed in kill round %d\n", round);
    return 1;
  }

  if (pid == 0) {
    char token = 'x';
    close(ready[0]);
    int rc = touch_pressure_mapping(100 + round);
    if (rc != 0)
      _exit(rc);
    if (write(ready[1], &token, 1) != 1)
      _exit(12);
    close(ready[1]);
    for (;;)
      ;
  }

  close(ready[1]);
  char token = 0;
  ssize_t n = read(ready[0], &token, 1);
  close(ready[0]);
  if (n != 1) {
    waitpid(pid, &status, 0);
    printf("[FAIL] mempressure_recovery: child not ready in kill round %d\n",
           round);
    return 1;
  }

  if (kill(pid, SIGKILL) != 0) {
    printf("[FAIL] mempressure_recovery: SIGKILL failed in round %d\n", round);
    return 1;
  }

  if (waitpid(pid, &status, 0) != pid) {
    printf("[FAIL] mempressure_recovery: waitpid failed in kill round %d\n",
           round);
    return 1;
  }

  if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
    printf("[FAIL] mempressure_recovery: kill round %d status=0x%x\n",
           round, status);
    return 1;
  }

  printf("[PASS] mempressure_recovery: kill round %d recycled pages\n", round);
  return 0;
}

int main(void)
{
  printf("mempressure_recovery: %lu MiB children under low memory\n",
         (unsigned long)PRESSURE_MB);
  alarm(180);

  for (int i = 0; i < CYCLES; i++) {
    if (run_exit_child(i) != 0)
      return 1;
    if (run_kill_child(i) != 0)
      return 1;
  }

  if (run_exit_child(1000) != 0)
    return 1;

  printf("[PASS] mempressure_recovery: low-memory recovery completed\n");
  return 0;
}
