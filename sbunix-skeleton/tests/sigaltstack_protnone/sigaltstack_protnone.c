#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define STACK_SIZE SIGSTKSZ

static void handler(int sig) {
  (void)sig;
  _exit(44);
}

int main(void) {
  printf("=== sigaltstack_protnone ===\n");

  pid_t pid = fork();
  if (pid == 0) {
    void *stk = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stk == MAP_FAILED)
      _exit(2);

    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = stk;
    ss.ss_size = STACK_SIZE;
    if (sigaltstack(&ss, NULL) < 0)
      _exit(3);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) < 0)
      _exit(4);

    if (mprotect(stk, STACK_SIZE, PROT_NONE) < 0)
      _exit(5);

    raise(SIGUSR1);
    _exit(6);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV) {
    printf("[PASS] sigaltstack_protnone\n");
    return 0;
  }

  printf("[FAIL] sigaltstack_protnone: status=0x%x\n", status);
  return 1;
}
