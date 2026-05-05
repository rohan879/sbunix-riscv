#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t usr1_count;
static volatile sig_atomic_t cont_pending_seen;

static int has_signal(sigset_t set, int sig) {
  return (set & (1UL << (sig - 1))) != 0;
}

static void usr1_handler(int sig) {
  (void)sig;
  usr1_count++;
}

static void usr2_handler(int sig) {
  sigset_t pending;

  (void)sig;
  cont_pending_seen = -1;
  if (kill(getpid(), SIGCONT) < 0)
    return;
  sigemptyset(&pending);
  if (sigpending(&pending) < 0)
    return;
  if (has_signal(pending, SIGCONT))
    cont_pending_seen = 1;
}

int main(void) {
  int pass = 1;
  struct sigaction sa;
  sigset_t block;
  sigset_t pending;
  sigset_t oldmask;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = usr1_handler;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGUSR1, &sa, 0) < 0) {
    printf("[FAIL] sigmask_pending: sigaction errno=%d\n", errno);
    return 1;
  }

  sigemptyset(&block);
  sigaddset(&block, SIGUSR1);
  sigemptyset(&oldmask);
  if (pthread_sigmask(SIG_BLOCK, &block, &oldmask) != 0) {
    printf("[FAIL] sigmask_pending: pthread_sigmask block failed\n");
    return 1;
  }
  if (sigprocmask(SIG_SETMASK, &oldmask, 0) < 0) {
    printf("[FAIL] sigmask_pending: mask restore errno=%d\n", errno);
    return 1;
  }
  errno = 123;
  if (pthread_sigmask(99, &block, 0) != EINVAL || errno != 123) {
    printf("[FAIL] sigmask_pending: pthread_sigmask invalid errno=%d\n",
           errno);
    return 1;
  }

  if (sigprocmask(SIG_BLOCK, &block, 0) < 0) {
    printf("[FAIL] sigmask_pending: block errno=%d\n", errno);
    return 1;
  }

  usr1_count = 0;
  if (raise(SIGUSR1) < 0) {
    printf("[FAIL] sigmask_pending: raise errno=%d\n", errno);
    return 1;
  }
  if (usr1_count != 0) {
    printf("[FAIL] sigmask_pending: raise delivered while blocked\n");
    pass = 0;
  }

  sigemptyset(&pending);
  if (sigpending(&pending) < 0) {
    printf("[FAIL] sigmask_pending: sigpending after raise errno=%d\n", errno);
    return 1;
  }
  if (!has_signal(pending, SIGUSR1)) {
    printf("[FAIL] sigmask_pending: raise not pending\n");
    pass = 0;
  }

  if (kill(getpid(), SIGUSR1) < 0) {
    printf("[FAIL] sigmask_pending: kill self errno=%d\n", errno);
    return 1;
  }
  if (usr1_count != 0) {
    printf("[FAIL] sigmask_pending: kill delivered while blocked\n");
    pass = 0;
  }

  sigemptyset(&pending);
  if (sigpending(&pending) < 0) {
    printf("[FAIL] sigmask_pending: sigpending after kill errno=%d\n", errno);
    return 1;
  }
  if (!has_signal(pending, SIGUSR1)) {
    printf("[FAIL] sigmask_pending: kill not pending\n");
    pass = 0;
  }

  if (sigprocmask(SIG_UNBLOCK, &block, 0) < 0) {
    printf("[FAIL] sigmask_pending: unblock errno=%d\n", errno);
    return 1;
  }
  if (usr1_count != 1) {
    printf("[FAIL] sigmask_pending: delivery count after unblock=%d\n",
           (int)usr1_count);
    pass = 0;
  }

  sigemptyset(&pending);
  if (sigpending(&pending) < 0) {
    printf("[FAIL] sigmask_pending: final sigpending errno=%d\n", errno);
    return 1;
  }
  if (has_signal(pending, SIGUSR1)) {
    printf("[FAIL] sigmask_pending: still pending after delivery\n");
    pass = 0;
  }

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = usr2_handler;
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGCONT);
  if (sigaction(SIGUSR2, &sa, 0) < 0) {
    printf("[FAIL] sigmask_pending: sigaction SIGUSR2 errno=%d\n", errno);
    return 1;
  }

  cont_pending_seen = 0;
  if (raise(SIGUSR2) < 0) {
    printf("[FAIL] sigmask_pending: raise SIGUSR2 errno=%d\n", errno);
    return 1;
  }
  if (cont_pending_seen != 1) {
    printf("[FAIL] sigmask_pending: masked SIGCONT pending=%d\n",
           (int)cont_pending_seen);
    pass = 0;
  }

  if (pass)
    printf("[PASS] sigmask_pending: blocked signals pend and deliver once\n");
  return pass ? 0 : 1;
}
