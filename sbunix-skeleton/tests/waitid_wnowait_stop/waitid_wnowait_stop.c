#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void child_loop(void) {
  for (;;) {
    struct timespec ts;
    ts.tv_sec = 1;
    ts.tv_nsec = 0;
    nanosleep(&ts, 0);
  }
}

static int check_stopped(const char *label, siginfo_t *si, pid_t child) {
  if (si->si_signo != SIGCHLD || si->si_code != CLD_STOPPED ||
      si->si_pid != child || si->si_status != SIGSTOP) {
    printf("[FAIL] waitid_wnowait_stop: %s signo=%d code=%d pid=%d status=%d\n",
           label, si->si_signo, si->si_code, si->si_pid, si->si_status);
    return 0;
  }
  printf("[PASS] waitid_wnowait_stop: %s reported stop\n", label);
  return 1;
}

int main(void) {
  int pass = 1;
  pid_t child = fork();
  if (child < 0) {
    printf("[FAIL] waitid_wnowait_stop: fork errno=%d\n", errno);
    return 1;
  }

  if (child == 0)
    child_loop();

  usleep(50000);
  if (kill(child, SIGSTOP) < 0) {
    printf("[FAIL] waitid_wnowait_stop: SIGSTOP errno=%d\n", errno);
    kill(child, SIGKILL);
    waitpid(child, 0, 0);
    return 1;
  }

  siginfo_t si;
  memset(&si, 0, sizeof(si));
  if (waitid(P_PID, child, &si, WSTOPPED | WNOWAIT) < 0) {
    printf("[FAIL] waitid_wnowait_stop: first waitid errno=%d\n", errno);
    pass = 0;
  } else if (!check_stopped("WNOWAIT first", &si, child)) {
    pass = 0;
  }

  memset(&si, 0, sizeof(si));
  if (waitid(P_PID, child, &si, WSTOPPED | WNOHANG | WNOWAIT) < 0) {
    printf("[FAIL] waitid_wnowait_stop: second waitid errno=%d\n", errno);
    pass = 0;
  } else if (!check_stopped("WNOWAIT second", &si, child)) {
    pass = 0;
  }

  memset(&si, 0, sizeof(si));
  if (waitid(P_PID, child, &si, WSTOPPED) < 0) {
    printf("[FAIL] waitid_wnowait_stop: consume waitid errno=%d\n", errno);
    pass = 0;
  } else if (!check_stopped("consume", &si, child)) {
    pass = 0;
  }

  memset(&si, 0, sizeof(si));
  if (waitid(P_PID, child, &si, WSTOPPED | WNOHANG) < 0) {
    printf("[FAIL] waitid_wnowait_stop: post-consume waitid errno=%d\n", errno);
    pass = 0;
  } else if (si.si_pid != 0) {
    printf("[FAIL] waitid_wnowait_stop: stop still visible after consume pid=%d\n",
           si.si_pid);
    pass = 0;
  } else {
    printf("[PASS] waitid_wnowait_stop: stop consumed without WNOWAIT\n");
  }

  kill(child, SIGKILL);
  int status = 0;
  if (waitpid(child, &status, 0) != child ||
      !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
    printf("[FAIL] waitid_wnowait_stop: cleanup status=0x%x errno=%d\n",
           status, errno);
    pass = 0;
  }

  return pass ? 0 : 1;
}
