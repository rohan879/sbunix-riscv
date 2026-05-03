#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void short_pause(void) {
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 20000000L;
  nanosleep(&ts, 0);
}

int main(void) {
  int fds[2];
  if (pipe(fds) < 0) {
    printf("[FAIL] sigmask_stop: pipe failed errno=%d\n", errno);
    return 1;
  }

  pid_t child = fork();
  if (child < 0) {
    printf("[FAIL] sigmask_stop: fork failed errno=%d\n", errno);
    return 1;
  }

  if (child == 0) {
    close(fds[0]);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTSTP);
    if (sigprocmask(SIG_BLOCK, &set, 0) < 0)
      _exit(2);

    char ready = 'r';
    if (write(fds[1], &ready, 1) != 1)
      _exit(3);

    for (;;) {
      struct timespec ts;
      ts.tv_sec = 1;
      ts.tv_nsec = 0;
      nanosleep(&ts, 0);
    }
  }

  close(fds[1]);
  char ready = 0;
  if (read(fds[0], &ready, 1) != 1 || ready != 'r') {
    kill(child, SIGKILL);
    waitpid(child, 0, 0);
    printf("[FAIL] sigmask_stop: child did not report ready\n");
    return 1;
  }

  if (kill(child, SIGTSTP) < 0) {
    kill(child, SIGKILL);
    waitpid(child, 0, 0);
    printf("[FAIL] sigmask_stop: SIGTSTP send failed errno=%d\n", errno);
    return 1;
  }

  short_pause();

  int status = 0;
  pid_t got = waitpid(child, &status, WNOHANG | WUNTRACED);
  if (got == child && WIFSTOPPED(status)) {
    kill(child, SIGKILL);
    waitpid(child, 0, 0);
    printf("[FAIL] sigmask_stop: blocked SIGTSTP stopped child\n");
    return 1;
  }
  if (got == child) {
    printf("[FAIL] sigmask_stop: child exited unexpectedly status=0x%x\n",
           status);
    return 1;
  }
  if (got < 0) {
    kill(child, SIGKILL);
    waitpid(child, 0, 0);
    printf("[FAIL] sigmask_stop: waitpid failed errno=%d\n", errno);
    return 1;
  }

  kill(child, SIGKILL);
  if (waitpid(child, &status, 0) != child ||
      !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
    printf("[FAIL] sigmask_stop: cleanup wait status=0x%x\n", status);
    return 1;
  }

  printf("[PASS] sigmask_stop: blocked job-control stop remains pending\n");
  return 0;
}
