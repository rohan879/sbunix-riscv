/*
 * mapshared_fork -- writable MAP_SHARED pages stay shared across fork while
 * read-only mappings remain protected.
 */

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define PAGE_SIZE_BYTES 4096UL
#define SEGV_SENTINEL 77

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(label, cond) do { \
  if (cond) { \
    printf("[PASS] mapshared_fork: %s\n", label); \
    g_pass++; \
  } else { \
    printf("[FAIL] mapshared_fork: %s\n", label); \
    g_fail++; \
  } \
} while (0)

static void segv_handler(int sig) {
  (void)sig;
  _exit(SEGV_SENTINEL);
}

static void install_segv_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = segv_handler;
  sigaction(SIGSEGV, &sa, 0);
}

static int write_pattern_file(const char *path, char value) {
  char buf[PAGE_SIZE_BYTES];
  memset(buf, value, sizeof(buf));
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd < 0)
    return -1;
  if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
    close(fd);
    return -1;
  }
  return fd;
}

static int wait_for_exit(pid_t pid, int want) {
  int status = 0;
  if (waitpid(pid, &status, 0) != pid)
    return 0;
  return WIFEXITED(status) && WEXITSTATUS(status) == want;
}

static void test_writable_shared_survives_fork(void) {
  const char *path = "/mnt/mapshared_fork_rw";
  int ok = 1;
  int fd = write_pattern_file(path, 'A');
  if (fd < 0) {
    CHECK("writable shared fork", 0);
    return;
  }

  char *p = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    unlink(path);
    CHECK("writable shared fork", 0);
    return;
  }

  p[0] = 'P';
  p[123] = 'Q';

  pid_t pid = fork();
  if (pid < 0) {
    ok = 0;
  } else if (pid == 0) {
    p[0] = 'C';
    p[123] = 'D';
    _exit(0);
  } else {
    ok = wait_for_exit(pid, 0);
    if (ok)
      ok = (p[0] == 'C' && p[123] == 'D');
  }

  munmap(p, PAGE_SIZE_BYTES);

  if (ok) {
    char buf[128];
    fd = open(path, O_RDONLY, 0);
    if (fd < 0 || read(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf))
      ok = 0;
    else
      ok = (buf[0] == 'C' && buf[123] == 'D');
    if (fd >= 0)
      close(fd);
  }

  unlink(path);
  CHECK("writable shared fork", ok);
}

static int expect_write_segv(char *p) {
  pid_t pid = fork();
  if (pid < 0)
    return 0;
  if (pid == 0) {
    install_segv_handler();
    p[0] = 'X';
    _exit(12);
  }
  return wait_for_exit(pid, SEGV_SENTINEL);
}

static void test_readonly_shared_stays_readonly(void) {
  const char *path = "/mnt/mapshared_fork_ro_shared";
  int fd = write_pattern_file(path, 'R');
  if (fd < 0) {
    CHECK("readonly shared negative", 0);
    return;
  }
  close(fd);

  fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    unlink(path);
    CHECK("readonly shared negative", 0);
    return;
  }

  char *p = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    unlink(path);
    CHECK("readonly shared negative", 0);
    return;
  }

  volatile char prefault = p[0];
  (void)prefault;
  int ok = expect_write_segv(p);
  munmap(p, PAGE_SIZE_BYTES);

  fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    ok = 0;
  } else {
    char c = 0;
    if (read(fd, &c, 1) != 1 || c != 'R')
      ok = 0;
    close(fd);
  }

  unlink(path);
  CHECK("readonly shared negative", ok);
}

static void test_readonly_private_stays_readonly(void) {
  const char *path = "/mnt/mapshared_fork_ro_private";
  int fd = write_pattern_file(path, 'P');
  if (fd < 0) {
    CHECK("readonly private negative", 0);
    return;
  }

  char *p = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    unlink(path);
    CHECK("readonly private negative", 0);
    return;
  }

  volatile char prefault = p[0];
  (void)prefault;
  int ok = expect_write_segv(p);
  munmap(p, PAGE_SIZE_BYTES);

  fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    ok = 0;
  } else {
    char c = 0;
    if (read(fd, &c, 1) != 1 || c != 'P')
      ok = 0;
    close(fd);
  }

  unlink(path);
  CHECK("readonly private negative", ok);
}

int main(void) {
  alarm(60);
  test_writable_shared_survives_fork();
  test_readonly_shared_stays_readonly();
  test_readonly_private_stays_readonly();

  if (g_fail == 0) {
    printf("[PASS] mapshared_fork\n");
    return 0;
  }

  printf("[FAIL] mapshared_fork: %d failed, %d passed\n", g_fail, g_pass);
  return 1;
}
