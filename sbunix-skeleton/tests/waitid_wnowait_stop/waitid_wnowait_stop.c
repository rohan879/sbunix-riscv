#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t got_chld;
static volatile sig_atomic_t got_alarm;
static volatile sig_atomic_t got_cont;
static volatile sig_atomic_t got_cont_pid;
static volatile sig_atomic_t got_cont_code;

static void chld_handler(int sig) {
  (void)sig;
  got_chld = 1;
}

static void alarm_handler(int sig) {
  (void)sig;
  got_alarm = 1;
}

static void chld_info_handler(int sig, siginfo_t *info, void *ctx) {
  (void)sig;
  (void)ctx;
  got_chld = 1;
  if (info) {
    got_cont_pid = info->si_pid;
    got_cont_code = info->si_code;
    if (info->si_code == CLD_CONTINUED)
      got_cont = 1;
  }
}

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

static int test_waitpid_stop_sigchld_eintr(void) {
  int fds[2];
  if (pipe(fds) != 0) {
    printf("[FAIL] waitid_wnowait_stop: waitpid pipe errno=%d\n", errno);
    return 0;
  }

  struct sigaction chld;
  struct sigaction old_chld;
  struct sigaction alrm;
  struct sigaction old_alrm;
  memset(&chld, 0, sizeof(chld));
  memset(&alrm, 0, sizeof(alrm));
  chld.sa_handler = chld_handler;
  alrm.sa_handler = alarm_handler;
  sigemptyset(&chld.sa_mask);
  sigemptyset(&alrm.sa_mask);
  sigaction(SIGCHLD, &chld, &old_chld);
  sigaction(SIGALRM, &alrm, &old_alrm);
  got_chld = 0;
  got_alarm = 0;

  pid_t child = fork();
  if (child < 0) {
    printf("[FAIL] waitid_wnowait_stop: waitpid fork errno=%d\n", errno);
    close(fds[0]);
    close(fds[1]);
    sigaction(SIGCHLD, &old_chld, 0);
    sigaction(SIGALRM, &old_alrm, 0);
    return 0;
  }
  if (child == 0) {
    close(fds[0]);
    char ready = 'r';
    write(fds[1], &ready, 1);
    close(fds[1]);
    usleep(50000);
    kill(getpid(), SIGSTOP);
    _exit(0);
  }

  close(fds[1]);
  char ready = 0;
  read(fds[0], &ready, 1);
  close(fds[0]);

  int status = 0;
  alarm(2);
  errno = 0;
  pid_t got = waitpid(child, &status, 0);
  alarm(0);
  int ok = (got == -1 && errno == EINTR && got_chld && !got_alarm);
  if (ok)
    printf("[PASS] waitid_wnowait_stop: caught SIGCHLD interrupts unreportable waitpid stop\n");
  else
    printf("[FAIL] waitid_wnowait_stop: waitpid got=%d errno=%d chld=%d alarm=%d\n",
           got, errno, got_chld, got_alarm);

  kill(child, SIGKILL);
  waitpid(child, &status, 0);
  sigaction(SIGCHLD, &old_chld, 0);
  sigaction(SIGALRM, &old_alrm, 0);
  return ok;
}

static int test_sigcont_cld_continued(void) {
  struct sigaction chld;
  struct sigaction old_chld;
  memset(&chld, 0, sizeof(chld));
  chld.sa_sigaction = chld_info_handler;
  chld.sa_flags = SA_SIGINFO;
  sigemptyset(&chld.sa_mask);
  sigaction(SIGCHLD, &chld, &old_chld);
  got_chld = 0;
  got_cont = 0;
  got_cont_pid = 0;
  got_cont_code = 0;

  pid_t child = fork();
  if (child < 0) {
    printf("[FAIL] waitid_wnowait_stop: cont fork errno=%d\n", errno);
    sigaction(SIGCHLD, &old_chld, 0);
    return 0;
  }
  if (child == 0)
    child_loop();

  usleep(50000);
  kill(child, SIGSTOP);
  siginfo_t si;
  memset(&si, 0, sizeof(si));
  if (waitid(P_PID, child, &si, WSTOPPED) < 0) {
    printf("[FAIL] waitid_wnowait_stop: cont stop wait errno=%d\n", errno);
    kill(child, SIGKILL);
    waitpid(child, 0, 0);
    sigaction(SIGCHLD, &old_chld, 0);
    return 0;
  }

  got_chld = 0;
  got_cont = 0;
  got_cont_pid = 0;
  got_cont_code = 0;
  kill(child, SIGCONT);
  for (int i = 0; i < 200 && !got_cont; i++)
    usleep(10000);

  int ok = (got_cont && got_cont_pid == child &&
            got_cont_code == CLD_CONTINUED);
  if (ok)
    printf("[PASS] waitid_wnowait_stop: default SIGCONT reports CLD_CONTINUED\n");
  else
    printf("[FAIL] waitid_wnowait_stop: cont pid=%d code=%d got=%d\n",
           got_cont_pid, got_cont_code, got_cont);

  kill(child, SIGKILL);
  waitpid(child, 0, 0);
  sigaction(SIGCHLD, &old_chld, 0);
  return ok;
}

static int test_waitpid_wcontinued(void) {
  pid_t child = fork();
  if (child < 0) {
    printf("[FAIL] waitid_wnowait_stop: WCONTINUED fork errno=%d\n", errno);
    return 0;
  }
  if (child == 0)
    child_loop();

  usleep(50000);
  kill(child, SIGSTOP);
  int status = 0;
  if (waitpid(child, &status, WUNTRACED) != child || !WIFSTOPPED(status)) {
    printf("[FAIL] waitid_wnowait_stop: WCONTINUED stop status=0x%x errno=%d\n",
           status, errno);
    kill(child, SIGKILL);
    waitpid(child, 0, 0);
    return 0;
  }

  kill(child, SIGCONT);
  status = 0;
  int ok = (waitpid(child, &status, WCONTINUED) == child &&
            WIFCONTINUED(status));
  if (ok)
    printf("[PASS] waitid_wnowait_stop: waitpid reports WCONTINUED\n");
  else
    printf("[FAIL] waitid_wnowait_stop: WCONTINUED status=0x%x errno=%d\n",
           status, errno);

  kill(child, SIGKILL);
  waitpid(child, 0, 0);
  return ok;
}

static int test_sigcont_running_no_cld(void) {
  struct sigaction chld;
  struct sigaction old_chld;
  memset(&chld, 0, sizeof(chld));
  chld.sa_sigaction = chld_info_handler;
  chld.sa_flags = SA_SIGINFO;
  sigemptyset(&chld.sa_mask);
  sigaction(SIGCHLD, &chld, &old_chld);
  got_chld = 0;
  got_cont = 0;
  got_cont_pid = 0;
  got_cont_code = 0;

  pid_t child = fork();
  if (child < 0) {
    printf("[FAIL] waitid_wnowait_stop: running cont fork errno=%d\n", errno);
    sigaction(SIGCHLD, &old_chld, 0);
    return 0;
  }
  if (child == 0)
    child_loop();

  usleep(50000);
  kill(child, SIGCONT);
  for (int i = 0; i < 50 && !got_cont; i++)
    usleep(10000);

  int ok = !got_cont;
  if (ok)
    printf("[PASS] waitid_wnowait_stop: running SIGCONT has no CLD_CONTINUED\n");
  else
    printf("[FAIL] waitid_wnowait_stop: running cont pid=%d code=%d\n",
           got_cont_pid, got_cont_code);

  kill(child, SIGKILL);
  waitpid(child, 0, 0);
  sigaction(SIGCHLD, &old_chld, 0);
  return ok;
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

  if (!test_waitpid_stop_sigchld_eintr())
    pass = 0;
  if (!test_sigcont_cld_continued())
    pass = 0;
  if (!test_waitpid_wcontinued())
    pass = 0;
  if (!test_sigcont_running_no_cld())
    pass = 0;

  return pass ? 0 : 1;
}
