#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE 4096

static int check_reserved(void *addr) {
  errno = 0;
  int r = mprotect(addr, PAGE, PROT_READ);
  if (r == -1 && errno == EINVAL)
    return 1;
  printf("[FAIL] mprotect_kernel_addr: addr=%p r=%d errno=%d\n",
         addr, r, errno);
  return 0;
}

static int check_lifecycle(void) {
  void *p = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    return 0;
  if (mprotect(p, PAGE, PROT_NONE) < 0)
    return 0;

  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = { "true", NULL };
    execv("/bin/true", argv);
    _exit(73);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  ok = ok && munmap(p, 2 * PAGE) == 0;
  return ok;
}

int main(void) {
  int ok = 1;
  printf("=== mprotect_kernel_addr ===\n");
  ok = check_reserved((void *)0x10000000UL) && ok;
  ok = check_reserved((void *)0x101000UL) && ok;
  ok = check_reserved((void *)0x80000000UL) && ok;
  ok = check_lifecycle() && ok;

  if (ok) {
    printf("[PASS] mprotect_kernel_addr\n");
    return 0;
  }
  return 1;
}
