/*
 * sigsuspend_mask -- validate that sigsuspend restores the original
 * signal mask after the signal handler returns.
 *
 * POSIX requires that sigsuspend atomically replaces the process signal
 * mask with the provided mask, waits for a signal, invokes the handler,
 * and then restores the original mask before returning.  A previous bug
 * caused the temporary mask to persist after sigsuspend returned.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <errno.h>

static volatile sig_atomic_t handler_ran = 0;
static volatile sig_atomic_t handler_signum = 0;

/* Simple SIGUSR1 handler that records invocation */
static void sigusr1_handler(int sig) {
  handler_ran = 1;
  handler_signum = sig;
}

int main(void) {
  int pass = 1;
  sigset_t block_set, old_set, suspend_set, check_set;

  /* Install handler for SIGUSR1 */
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sigusr1_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0; /* No SA_RESTART -- sigsuspend must not restart */
  if (sigaction(SIGUSR1, &sa, NULL) < 0) {
    printf("[FAIL] sigsuspend_mask_restore (sigaction failed)\n");
    return 1;
  }

  /* Block SIGUSR1 in the current mask */
  sigemptyset(&block_set);
  sigaddset(&block_set, SIGUSR1);
  if (sigprocmask(SIG_BLOCK, &block_set, &old_set) < 0) {
    printf("[FAIL] sigsuspend_mask_restore (sigprocmask SIG_BLOCK failed)\n");
    return 1;
  }

  /* Fork a child that will send us SIGUSR1 after a short delay */
  pid_t child = fork();
  if (child < 0) {
    printf("[FAIL] sigsuspend_mask_restore (fork failed)\n");
    return 1;
  }

  if (child == 0) {
    /* Child: small delay then send SIGUSR1 to parent */
    pid_t ppid = getppid();
    /* Busy-wait a bit to let parent reach sigsuspend */
    for (volatile int i = 0; i < 100000; i++)
      ;
    kill(ppid, SIGUSR1);
    _exit(0);
  }

  /* Parent: sigsuspend with a mask that UNblocks SIGUSR1 */
  sigemptyset(&suspend_set);
  /* suspend_set is empty, so SIGUSR1 is NOT blocked during suspend */

  handler_ran = 0;
  int ret = sigsuspend(&suspend_set);
  /* sigsuspend always returns -1 with errno == EINTR */

  if (!handler_ran) {
    printf("[FAIL] sigsuspend_mask_restore (handler did not run)\n");
    pass = 0;
  }

  if (handler_signum != SIGUSR1) {
    printf("[FAIL] sigsuspend_mask_restore (wrong signal: %d)\n",
           handler_signum);
    pass = 0;
  }

  /* The critical check: after sigsuspend returns, the original mask
   * (with SIGUSR1 blocked) must be restored.  If the deferred mask
   * restore bug is present, the temporary mask (empty) persists. */
  if (sigprocmask(SIG_SETMASK, NULL, &check_set) < 0) {
    printf("[FAIL] sigsuspend_mask_restore (sigprocmask read failed)\n");
    pass = 0;
  } else {
    /* Verify SIGUSR1 is still blocked in the restored mask */
    int is_blocked = 0;
    /* sigset_t is uint64_t in our libc; check the bit directly */
    if (check_set & (1UL << (SIGUSR1 - 1))) {
      is_blocked = 1;
    }
    if (!is_blocked) {
      printf("[FAIL] sigsuspend_mask_restore "
             "(SIGUSR1 not blocked after sigsuspend returned)\n");
      pass = 0;
    }
  }

  /* Reap child */
  int status;
  waitpid(child, &status, 0);

  if (pass) {
    printf("[PASS] sigsuspend_mask_restore\n");
  }

  return pass ? 0 : 1;
}
