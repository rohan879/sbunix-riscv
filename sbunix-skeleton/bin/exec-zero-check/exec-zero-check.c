#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define DIRTY_PAGES 64

static int dirty_child(int round) {
  size_t len = DIRTY_PAGES * PAGE_SIZE;
  unsigned char *region = mmap(NULL, len, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED)
    return 10 + round;

  for (size_t i = 0; i < len; i++)
    region[i] = (unsigned char)(0x30 + round);

  {
    char stack_buf[PAGE_SIZE];
    memset(stack_buf, 0x80 + round, sizeof(stack_buf));
    if (stack_buf[0] == 0)
      return 20 + round;
  }

  return 0;
}

int main(void) {
  int status;
  int pfd[2];
  char out[128];
  ssize_t nr;

  printf("exec-zero-check: dirtying freed pages before exec\n");

  for (int round = 0; round < 6; round++) {
    pid_t child = fork();
    if (child < 0) {
      printf("[FAIL] fork dirty child\n");
      return 1;
    }
    if (child == 0)
      _exit(dirty_child(round));

    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      printf("[FAIL] dirty child round %d status=0x%x\n", round, status);
      return 1;
    }
  }

  if (pipe(pfd) != 0) {
    printf("[FAIL] pipe\n");
    return 1;
  }

  pid_t exec_child = fork();
  if (exec_child < 0) {
    printf("[FAIL] fork exec child\n");
    return 1;
  }
  if (exec_child == 0) {
    char *argv[] = {"echo", "exec-zero-ok", NULL};
    close(pfd[0]);
    dup2(pfd[1], STDOUT_FILENO);
    close(pfd[1]);
    execv("/bin/echo", argv);
    _exit(127);
  }

  close(pfd[1]);
  nr = read(pfd[0], out, sizeof(out) - 1);
  close(pfd[0]);
  if (nr < 0)
    nr = 0;
  out[nr] = '\0';

  waitpid(exec_child, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    printf("[FAIL] exec child status=0x%x\n", status);
    return 1;
  }
  if (strcmp(out, "exec-zero-ok\n") != 0) {
    printf("[FAIL] unexpected output '%s'\n", out);
    return 1;
  }

  printf("[PASS] exec-zero-check\n");
  return 0;
}
