#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t usr1_seen;
static volatile sig_atomic_t alarm_seen;

static void usr1_handler(int sig) {
  (void)sig;
  usr1_seen = 1;
}

static void alarm_handler(int sig) {
  (void)sig;
  alarm_seen = 1;
}

static int child_main(int use_timer) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = usr1_handler;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGUSR1, &sa, NULL) < 0)
    return 10;

  for (int i = 0; i < 128; i++) {
    struct timeval tv;
    if (getpid() <= 0 || getppid() <= 0)
      return 11;
    if (gettimeofday(&tv, NULL) < 0)
      return 12;
  }

  raise(SIGUSR1);
  if (!usr1_seen)
    return 13;

  if (use_timer) {
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = alarm_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, NULL) < 0)
      return 14;
    alarm(1);
    while (!alarm_seen)
      pause();
  }

  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--child") == 0)
    return child_main(argc > 2 && strcmp(argv[2], "timer") == 0);

  printf("=== trapretstress ===\n");
  for (int i = 0; i < 12; i++) {
    pid_t pid = fork();
    if (pid == 0) {
      char *args_timer[] = { "trapretstress", "--child", "timer", NULL };
      char *args_plain[] = { "trapretstress", "--child", NULL };
      execv("/test/bin/trapretstress", i == 0 ? args_timer : args_plain);
      _exit(90);
    }
    if (pid < 0) {
      printf("[FAIL] trapretstress: fork %d\n", i);
      return 1;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      printf("[FAIL] trapretstress: child %d status=0x%x\n", i, status);
      return 1;
    }
  }
  printf("[PASS] trapretstress\n");
  return 0;
}
