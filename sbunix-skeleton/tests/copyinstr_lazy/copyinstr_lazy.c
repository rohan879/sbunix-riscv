#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE 4096

static char *lazy_string(const char *s) {
  int pfd[2];
  char *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    return MAP_FAILED;
  if (pipe(pfd) < 0)
    return MAP_FAILED;
  int len = (int)strlen(s) + 1;
  if (write(pfd[1], s, len) != len)
    return MAP_FAILED;
  if (read(pfd[0], p, len) != len)
    return MAP_FAILED;
  close(pfd[0]);
  close(pfd[1]);
  return p;
}

static int test_exec_strings(void) {
  char *path = lazy_string("/bin/true");
  char *arg0 = lazy_string("true");
  char *env0 = lazy_string("COPYINSTR_LAZY=1");
  if (path == MAP_FAILED || arg0 == MAP_FAILED || env0 == MAP_FAILED)
    return 0;

  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = { arg0, NULL };
    char *envp[] = { env0, NULL };
    execve(path, argv, envp);
    _exit(77);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int test_protnone_string_fails(void) {
  char *path = lazy_string("/bin/true");
  if (path == MAP_FAILED)
    return 0;
  if (mprotect(path, PAGE, PROT_NONE) < 0)
    return 0;

  errno = 0;
  int fd = open(path, O_RDONLY);
  int ok = (fd < 0 && errno == EFAULT);
  if (fd >= 0)
    close(fd);

  if (mprotect(path, PAGE, PROT_READ) < 0)
    return 0;
  ok = ok && strcmp(path, "/bin/true") == 0;
  munmap(path, PAGE);
  return ok;
}

int main(void) {
  printf("=== copyinstr_lazy ===\n");
  if (!test_exec_strings()) {
    printf("[FAIL] copyinstr_lazy: exec strings\n");
    return 1;
  }
  if (!test_protnone_string_fails()) {
    printf("[FAIL] copyinstr_lazy: PROT_NONE string\n");
    return 1;
  }
  printf("[PASS] copyinstr_lazy\n");
  return 0;
}
