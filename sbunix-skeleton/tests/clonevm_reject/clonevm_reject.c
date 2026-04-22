#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CLONE_VM_LOCAL 0x00000100

int main(void) {
  long ret = __syscall_ret(syscall(SYS_clone, CLONE_VM_LOCAL | SIGCHLD,
                                   0, 0, 0, 0));
  if (ret == -1 && errno == EINVAL) {
    printf("[PASS] clonevm_reject\n");
    return 0;
  }

  if (ret == 0)
    _exit(99);

  printf("[FAIL] clonevm_reject ret=%ld errno=%d\n", ret, errno);
  return 1;
}
