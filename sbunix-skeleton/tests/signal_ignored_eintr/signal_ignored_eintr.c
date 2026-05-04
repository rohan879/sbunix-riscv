#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile int g_handled_usr1;

static void record_usr1(int sig) {
  (void)sig;
  g_handled_usr1++;
}

static void child_sleep_case(int sig, int block_signal, int ready_fd) {
  if (block_signal) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    if (pthread_sigmask(SIG_BLOCK, &set, 0) != 0) {
      printf("[FAIL] signal_ignored_eintr: pthread_sigmask block\n");
      _exit(2);
    }
  } else {
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_restorer = 0;
    if (sigaction(sig, &sa, 0) < 0) {
      printf("[FAIL] signal_ignored_eintr: sigaction errno=%d\n", errno);
      _exit(3);
    }
  }

  char ready = 'r';
  if (write(ready_fd, &ready, 1) != 1)
    _exit(4);
  close(ready_fd);

  struct timespec req;
  req.tv_sec = 0;
  req.tv_nsec = 250000000L;
  errno = 0;
  if (nanosleep(&req, 0) < 0) {
    printf("[FAIL] signal_ignored_eintr: nanosleep sig=%d errno=%d\n",
           sig, errno);
    _exit(5);
  }
  _exit(0);
}

static int run_sleep_case(const char *name, int sig, int block_signal) {
  int fds[2];
  if (pipe(fds) < 0) {
    printf("[FAIL] signal_ignored_eintr: %s pipe errno=%d\n", name, errno);
    return 0;
  }

  pid_t child = fork();
  if (child < 0) {
    printf("[FAIL] signal_ignored_eintr: %s fork errno=%d\n", name, errno);
    close(fds[0]);
    close(fds[1]);
    return 0;
  }

  if (child == 0) {
    close(fds[0]);
    child_sleep_case(sig, block_signal, fds[1]);
  }

  close(fds[1]);
  char ready = 0;
  if (read(fds[0], &ready, 1) != 1 || ready != 'r') {
    kill(child, SIGKILL);
    waitpid(child, 0, 0);
    close(fds[0]);
    printf("[FAIL] signal_ignored_eintr: %s child not ready\n", name);
    return 0;
  }
  close(fds[0]);

  usleep(50000);
  if (kill(child, sig) < 0) {
    kill(child, SIGKILL);
    waitpid(child, 0, 0);
    printf("[FAIL] signal_ignored_eintr: %s kill errno=%d\n", name, errno);
    return 0;
  }

  int status = 0;
  if (waitpid(child, &status, 0) != child) {
    printf("[FAIL] signal_ignored_eintr: %s wait errno=%d\n", name, errno);
    return 0;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    printf("[FAIL] signal_ignored_eintr: %s status=0x%x\n", name, status);
    return 0;
  }

  printf("[PASS] signal_ignored_eintr: %s did not return EINTR\n", name);
  return 1;
}

static int run_ppoll_ignored_case(void) {
  int fds[2];
  if (pipe(fds) < 0) {
    printf("[FAIL] signal_ignored_eintr: ppoll pipe errno=%d\n", errno);
    return 0;
  }

  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sa.sa_restorer = 0;
  if (sigaction(SIGUSR1, &sa, 0) < 0) {
    printf("[FAIL] signal_ignored_eintr: ppoll sigaction errno=%d\n", errno);
    close(fds[0]);
    close(fds[1]);
    return 0;
  }

  pid_t parent = getpid();
  pid_t child = fork();
  if (child < 0) {
    printf("[FAIL] signal_ignored_eintr: ppoll fork errno=%d\n", errno);
    close(fds[0]);
    close(fds[1]);
    return 0;
  }
  if (child == 0) {
    close(fds[0]);
    usleep(50000);
    kill(parent, SIGUSR1);
    usleep(50000);
    write(fds[1], "q", 1);
    close(fds[1]);
    _exit(0);
  }

  close(fds[1]);
  struct pollfd pfd;
  pfd.fd = fds[0];
  pfd.events = POLLIN;
  pfd.revents = 0;
  struct timespec ts;
  ts.tv_sec = 1;
  ts.tv_nsec = 0;
  errno = 0;
  int ret = ppoll(&pfd, 1, &ts, 0);
  char c = 0;
  int ok = (ret == 1 && (pfd.revents & POLLIN) &&
            read(fds[0], &c, 1) == 1 && c == 'q');
  if (ok)
    printf("[PASS] signal_ignored_eintr: ppoll ignored signal did not return EINTR\n");
  else
    printf("[FAIL] signal_ignored_eintr: ppoll ret=%d revents=%d errno=%d\n",
           ret, pfd.revents, errno);

  close(fds[0]);
  int status = 0;
  waitpid(child, &status, 0);
  return ok;
}

static int run_sigtimedwait_restart_case(void) {
  struct sigaction sa;
  sa.sa_handler = record_usr1;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sa.sa_restorer = 0;
  if (sigaction(SIGUSR1, &sa, 0) < 0) {
    printf("[FAIL] signal_ignored_eintr: sigtimedwait sigaction errno=%d\n", errno);
    return 0;
  }

  pid_t parent = getpid();
  pid_t child = fork();
  if (child < 0) {
    printf("[FAIL] signal_ignored_eintr: sigtimedwait fork errno=%d\n", errno);
    return 0;
  }
  if (child == 0) {
    usleep(50000);
    kill(parent, SIGUSR1);
    _exit(0);
  }

  sigset_t waitset;
  sigemptyset(&waitset);
  sigaddset(&waitset, SIGUSR2);
  struct timespec ts;
  ts.tv_sec = 1;
  ts.tv_nsec = 0;
  g_handled_usr1 = 0;
  errno = 0;
  int ret = sigtimedwait(&waitset, 0, &ts);
  int ok = (ret < 0 && errno == EINTR && g_handled_usr1 == 1);
  if (ok)
    printf("[PASS] signal_ignored_eintr: sigtimedwait SA_RESTART still returns EINTR\n");
  else
    printf("[FAIL] signal_ignored_eintr: sigtimedwait ret=%d errno=%d handled=%d\n",
           ret, errno, g_handled_usr1);

  int status = 0;
  waitpid(child, &status, 0);
  return ok;
}

static int run_epoll_pwait_mask_restart_case(void) {
  int fds[2];
  if (pipe(fds) < 0) {
    printf("[FAIL] signal_ignored_eintr: epoll pipe errno=%d\n", errno);
    return 0;
  }

  long efd = syscall(SYS_epoll_create1, 0);
  if (efd < 0) {
    printf("[FAIL] signal_ignored_eintr: epoll create ret=%ld\n", efd);
    close(fds[0]);
    close(fds[1]);
    return 0;
  }

  uint8_t event[12];
  memset(event, 0, sizeof(event));
  uint32_t events = 0x01; /* EPOLLIN */
  uint64_t data = 0x506;
  memcpy(event, &events, 4);
  memcpy(event + 4, &data, 8);
  long ctl = syscall(SYS_epoll_ctl, efd, 1, fds[0], event);
  if (ctl < 0) {
    printf("[FAIL] signal_ignored_eintr: epoll ctl ret=%ld\n", ctl);
    close((int)efd);
    close(fds[0]);
    close(fds[1]);
    return 0;
  }

  struct sigaction sa;
  sa.sa_handler = record_usr1;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sa.sa_restorer = 0;
  if (sigaction(SIGUSR1, &sa, 0) < 0) {
    printf("[FAIL] signal_ignored_eintr: epoll sigaction errno=%d\n", errno);
    close((int)efd);
    close(fds[0]);
    close(fds[1]);
    return 0;
  }

  sigset_t block;
  sigset_t oldmask;
  sigemptyset(&block);
  sigaddset(&block, SIGUSR1);
  if (pthread_sigmask(SIG_BLOCK, &block, &oldmask) != 0) {
    printf("[FAIL] signal_ignored_eintr: epoll pthread_sigmask block\n");
    close((int)efd);
    close(fds[0]);
    close(fds[1]);
    return 0;
  }

  pid_t parent = getpid();
  pid_t child = fork();
  if (child < 0) {
    printf("[FAIL] signal_ignored_eintr: epoll fork errno=%d\n", errno);
    pthread_sigmask(SIG_SETMASK, &oldmask, 0);
    close((int)efd);
    close(fds[0]);
    close(fds[1]);
    return 0;
  }
  if (child == 0) {
    usleep(50000);
    kill(parent, SIGUSR1);
    _exit(0);
  }

  sigset_t empty;
  sigemptyset(&empty);
  uint8_t out[12];
  memset(out, 0, sizeof(out));
  g_handled_usr1 = 0;
  long ret = syscall(SYS_epoll_pwait, efd, out, 1, 1000, &empty, sizeof(empty));

  sigset_t curmask;
  pthread_sigmask(SIG_SETMASK, 0, &curmask);
  int restored = sigismember(&curmask, SIGUSR1) == 1;
  pthread_sigmask(SIG_SETMASK, &oldmask, 0);

  int status = 0;
  waitpid(child, &status, 0);
  close((int)efd);
  close(fds[0]);
  close(fds[1]);

  int ok = (ret == -EINTR && g_handled_usr1 == 1 && restored);
  if (ok)
    printf("[PASS] signal_ignored_eintr: epoll_pwait mask SA_RESTART returns EINTR\n");
  else
    printf("[FAIL] signal_ignored_eintr: epoll_pwait ret=%ld handled=%d restored=%d\n",
           ret, g_handled_usr1, restored);
  return ok;
}

int main(void) {
  int pass = 1;
  sigset_t set;

  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  errno = 321;
  if (pthread_sigmask(99, &set, 0) != EINVAL || errno != 321) {
    printf("[FAIL] signal_ignored_eintr: pthread_sigmask errno=%d\n", errno);
    pass = 0;
  } else {
    printf("[PASS] signal_ignored_eintr: pthread_sigmask preserves errno\n");
  }

  if (!run_sleep_case("ignored SIGUSR1", SIGUSR1, 0))
    pass = 0;
  if (!run_sleep_case("blocked SIGUSR2", SIGUSR2, 1))
    pass = 0;
  if (!run_ppoll_ignored_case())
    pass = 0;
  if (!run_sigtimedwait_restart_case())
    pass = 0;
  if (!run_epoll_pwait_mask_restart_case())
    pass = 0;

  return pass ? 0 : 1;
}
