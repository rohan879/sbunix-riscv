#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>

#define SIGSET_BYTES_MAX 128

static int passed;
static int failed;
static volatile sig_atomic_t usr2_count;

static uint64_t sig_bit(int sig) {
  return 1UL << (sig - 1);
}

static void check(const char *name, int ok) {
  if (ok) {
    printf("[PASS] sigmask_abi: %s\n", name);
    passed++;
  } else {
    printf("[FAIL] sigmask_abi: %s\n", name);
    failed++;
  }
}

static int bytes_are_zero(const unsigned char *buf, unsigned long start,
                          unsigned long end) {
  for (unsigned long i = start; i < end; i++) {
    if (buf[i] != 0)
      return 0;
  }
  return 1;
}

static int bytes_are_value(const unsigned char *buf, unsigned long end,
                           unsigned char val) {
  for (unsigned long i = 0; i < end; i++) {
    if (buf[i] != val)
      return 0;
  }
  return 1;
}

static uint64_t load_low_word(const unsigned char *buf) {
  uint64_t low = 0;
  memcpy(&low, buf, sizeof(low));
  return low;
}

static void store_low_word(unsigned char *buf, uint64_t low) {
  memcpy(buf, &low, sizeof(low));
}

static void usr2_handler(int sig) {
  (void)sig;
  usr2_count++;
}

static void reset_mask(sigset_t mask) {
  if (sigprocmask(SIG_SETMASK, &mask, 0) < 0) {
    printf("[FAIL] sigmask_abi: restore mask errno=%d\n", errno);
    failed++;
  }
}

static void test_libc_sanitizes_unblockable(sigset_t saved) {
  sigset_t full;
  sigset_t cur;

  sigfillset(&full);
  if (sigprocmask(SIG_SETMASK, &full, 0) < 0) {
    check("libc set full mask", 0);
    reset_mask(saved);
    return;
  }

  cur = 0;
  if (sigprocmask(SIG_SETMASK, 0, &cur) < 0) {
    check("libc read current mask", 0);
    reset_mask(saved);
    return;
  }

  check("libc clears SIGKILL and SIGSTOP from process mask",
        (cur & sig_bit(SIGKILL)) == 0 && (cur & sig_bit(SIGSTOP)) == 0);
  reset_mask(saved);
}

static void test_raw_sigprocmask_size(unsigned long size, sigset_t saved) {
  unsigned char in[SIGSET_BYTES_MAX];
  unsigned char old[SIGSET_BYTES_MAX];
  unsigned char cur[SIGSET_BYTES_MAX];
  sigset_t empty = 0;

  reset_mask(empty);

  memset(in, 0x5a, sizeof(in));
  memset(old, 0xa5, sizeof(old));
  uint64_t requested = sig_bit(SIGUSR1) | sig_bit(SIGKILL) |
                       sig_bit(SIGSTOP);
  store_low_word(in, requested);

  long r = syscall(SYS_rt_sigprocmask, SIG_SETMASK, in, old, size);
  check("raw rt_sigprocmask accepts supported size", r == 0);
  if (r != 0) {
    reset_mask(saved);
    return;
  }

  check("raw rt_sigprocmask old mask low word",
        load_low_word(old) == 0);
  if (size > sizeof(uint64_t)) {
    check("raw rt_sigprocmask old mask high bytes zero",
          bytes_are_zero(old, sizeof(uint64_t), size));
  }

  memset(cur, 0xcc, sizeof(cur));
  r = syscall(SYS_rt_sigprocmask, SIG_SETMASK, 0, cur, size);
  check("raw rt_sigprocmask reads current mask", r == 0);
  if (r == 0) {
    check("raw rt_sigprocmask copies only low 64 bits",
          load_low_word(cur) == sig_bit(SIGUSR1));
    if (size > sizeof(uint64_t)) {
      check("raw rt_sigprocmask current mask high bytes zero",
            bytes_are_zero(cur, sizeof(uint64_t), size));
    }
  }

  reset_mask(saved);
}

static void test_bad_raw_size(sigset_t saved) {
  sigset_t set = 0;
  long r = syscall(SYS_rt_sigprocmask, SIG_SETMASK, &set, 0, 24);
  check("raw rt_sigprocmask rejects unsupported size", r == -EINVAL);
  reset_mask(saved);
}

static void test_invalid_how_keeps_oldset(sigset_t saved) {
  unsigned char in[16];
  unsigned char old[16];
  unsigned char cur[16];
  sigset_t empty = 0;
  long r;

  reset_mask(empty);
  memset(in, 0, sizeof(in));
  memset(old, 0x9b, sizeof(old));
  store_low_word(in, sig_bit(SIGUSR1));

  r = syscall(SYS_rt_sigprocmask, 99, in, old, sizeof(old));
  check("raw rt_sigprocmask rejects invalid how", r == -EINVAL);
  check("raw rt_sigprocmask invalid how leaves oldset untouched",
        bytes_are_value(old, sizeof(old), 0x9b));

  memset(cur, 0xee, sizeof(cur));
  r = syscall(SYS_rt_sigprocmask, SIG_SETMASK, 0, cur, sizeof(cur));
  check("raw rt_sigprocmask invalid how leaves mask unchanged",
        r == 0 && load_low_word(cur) == 0);

  reset_mask(saved);
}

static void test_pending_wide_output(sigset_t saved) {
  struct sigaction sa;
  unsigned char block[SIGSET_BYTES_MAX];
  unsigned char pending[SIGSET_BYTES_MAX];
  uint64_t low = sig_bit(SIGUSR2);
  long r;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = usr2_handler;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGUSR2, &sa, 0) < 0) {
    check("install SIGUSR2 handler", 0);
    return;
  }

  usr2_count = 0;
  memset(block, 0x7e, sizeof(block));
  store_low_word(block, low);
  r = syscall(SYS_rt_sigprocmask, SIG_SETMASK, block, 0, 128);
  check("wide raw mask blocks SIGUSR2", r == 0);
  if (r != 0) {
    reset_mask(saved);
    return;
  }

  if (raise(SIGUSR2) < 0) {
    check("raise blocked SIGUSR2", 0);
    reset_mask(saved);
    return;
  }
  check("blocked SIGUSR2 not delivered immediately", usr2_count == 0);

  memset(pending, 0xd3, sizeof(pending));
  r = syscall(SYS_rt_sigpending, pending, 128);
  check("wide rt_sigpending succeeds", r == 0);
  if (r == 0) {
    check("wide rt_sigpending reports blocked signal",
          (load_low_word(pending) & low) != 0);
    check("wide rt_sigpending high bytes zero",
          bytes_are_zero(pending, sizeof(uint64_t), 128));
  }

  store_low_word(block, low);
  r = syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, block, 0, 16);
  check("raw unblock with 16-byte mask succeeds", r == 0);
  check("pending signal delivered after unblock", usr2_count == 1);

  reset_mask(saved);
}

int main(void) {
  sigset_t saved = 0;

  if (sigprocmask(SIG_SETMASK, 0, &saved) < 0) {
    printf("[FAIL] sigmask_abi: save mask errno=%d\n", errno);
    return 1;
  }

  test_libc_sanitizes_unblockable(saved);
  test_raw_sigprocmask_size(8, saved);
  test_raw_sigprocmask_size(16, saved);
  test_raw_sigprocmask_size(128, saved);
  test_bad_raw_size(saved);
  test_invalid_how_keeps_oldset(saved);
  test_pending_wide_output(saved);

  reset_mask(saved);
  printf("=== sigmask_abi: %d passed, %d failed ===\n", passed, failed);
  return failed ? 1 : 0;
}
