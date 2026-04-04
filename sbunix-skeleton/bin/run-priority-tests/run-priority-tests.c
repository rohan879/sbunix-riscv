#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

#define TIMEOUT_SEC 30

volatile pid_t g_child_pid;

static void alarm_handler(int sig) {
  (void)sig;
  if (g_child_pid > 0)
    kill(g_child_pid, SIGKILL);
}

static int run_test(const char *runner, const char *suite, const char *name) {
  pid_t pid = fork();
  if (pid < 0) {
    printf("[FAIL] %s/%s (fork failed)\n", suite, name);
    return -1;
  }
  if (pid == 0) {
    int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) {
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      close(fd);
    }
    char *argv[] = {(char *)runner, (char *)name, NULL};
    execve(runner, argv, NULL);
    _exit(127);
  }

  g_child_pid = pid;
  alarm(TIMEOUT_SEC);

  int status = 0;
  pid_t ret = waitpid(pid, &status, 0);
  alarm(0);
  g_child_pid = 0;

  if (ret < 0) {
    printf("[FAIL] %s/%s (waitpid error)\n", suite, name);
    return -1;
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    printf("[PASS] %s/%s\n", suite, name);
    return 0;
  }
  printf("[FAIL] %s/%s\n", suite, name);
  return -1;
}

static void run_manifest(const char *path, const char *runner,
                         const char *suite) {
  FILE *fp = fopen(path, "r");
  if (!fp)
    return;
  char line[512];
  while (fgets(line, sizeof(line), fp)) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    if (len == 0 || line[0] == '#')
      continue;
    run_test(runner, suite, line);
  }
  fclose(fp);
}

int main(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = alarm_handler;
  sigaction(SIGALRM, &sa, NULL);

  run_manifest("/test/priority-os-test-manifest",
               "/bin/run-os-test", "os-test");
  run_manifest("/test/priority-open-posix-manifest",
               "/bin/run-open-posix-test", "open-posix");
  return 0;
}
