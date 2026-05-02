/*
 * mapshared_lazy -- MAP_SHARED pages are shared before either side faults.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define PAGE 4096

static int write_file(const char *path, int pages, int tail) {
  char buf[PAGE];
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd < 0)
    return -1;
  for (int i = 0; i < pages; i++) {
    memset(buf, 'A' + i, sizeof(buf));
    if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
      close(fd);
      return -1;
    }
  }
  if (tail > 0) {
    memset(buf, 'z', (size_t)tail);
    if (write(fd, buf, (size_t)tail) != tail) {
      close(fd);
      return -1;
    }
  }
  close(fd);
  return 0;
}

static int wait_ok(pid_t pid) {
  int status = 0;
  return waitpid(pid, &status, 0) == pid &&
         WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int anon_fork_before_touch(void) {
  char *p = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  pid_t pid;
  int ok;

  if (p == MAP_FAILED)
    return 0;
  pid = fork();
  if (pid < 0) {
    munmap(p, 2 * PAGE);
    return 0;
  }
  if (pid == 0) {
    p[0] = 'c';
    p[PAGE + 9] = 'd';
    _exit(0);
  }
  ok = wait_ok(pid) && p[0] == 'c' && p[PAGE + 9] == 'd';
  munmap(p, 2 * PAGE);
  return ok;
}

static int file_separate_opens_before_touch(void) {
  const char *path = "/mnt/mapshared_lazy_file";
  int fd1, fd2, ok = 1;
  char *a;
  char *b;
  char check[16];

  if (write_file(path, 2, 0) < 0)
    return 0;
  fd1 = open(path, O_RDWR, 0);
  fd2 = open(path, O_RDWR, 0);
  if (fd1 < 0 || fd2 < 0) {
    if (fd1 >= 0) close(fd1);
    if (fd2 >= 0) close(fd2);
    unlink(path);
    return 0;
  }
  a = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd1, 0);
  b = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
  close(fd1);
  close(fd2);
  if (a == MAP_FAILED || b == MAP_FAILED) {
    if (a != MAP_FAILED) munmap(a, 2 * PAGE);
    if (b != MAP_FAILED) munmap(b, 2 * PAGE);
    unlink(path);
    return 0;
  }

  b[0] = 'x';
  ok = ok && a[0] == 'x';
  a[PAGE + 7] = 'y';
  ok = ok && b[PAGE + 7] == 'y';
  ok = ok && msync(a, 2 * PAGE, MS_SYNC) == 0;
  munmap(a, 2 * PAGE);
  munmap(b, 2 * PAGE);

  fd1 = open(path, O_RDONLY, 0);
  if (fd1 < 0) {
    unlink(path);
    return 0;
  }
  memset(check, 0, sizeof(check));
  ok = ok && read(fd1, check, sizeof(check)) == (ssize_t)sizeof(check);
  ok = ok && check[0] == 'x';
  close(fd1);
  unlink(path);
  return ok;
}

static int file_offset_partial_writeback(void) {
  const char *path = "/mnt/mapshared_lazy_partial";
  int fd;
  char *p;
  char c = 0;
  int ok = 1;

  if (write_file(path, 2, 123) < 0)
    return 0;
  fd = open(path, O_RDWR, 0);
  if (fd < 0) {
    unlink(path);
    return 0;
  }
  p = mmap(NULL, PAGE + 123, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PAGE);
  close(fd);
  if (p == MAP_FAILED) {
    unlink(path);
    return 0;
  }
  p[0] = 'm';
  p[PAGE + 122] = 'n';
  ok = ok && msync(p, PAGE + 123, MS_SYNC) == 0;
  munmap(p, PAGE + 123);

  fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    unlink(path);
    return 0;
  }
  lseek(fd, PAGE, SEEK_SET);
  ok = ok && read(fd, &c, 1) == 1 && c == 'm';
  lseek(fd, 2 * PAGE + 122, SEEK_SET);
  ok = ok && read(fd, &c, 1) == 1 && c == 'n';
  close(fd);
  unlink(path);
  return ok;
}

int main(void) {
  if (!anon_fork_before_touch()) {
    printf("[FAIL] mapshared_lazy: anon fork before touch\n");
    return 1;
  }
  if (!file_separate_opens_before_touch()) {
    printf("[FAIL] mapshared_lazy: file separate opens\n");
    return 1;
  }
  if (!file_offset_partial_writeback()) {
    printf("[FAIL] mapshared_lazy: offset partial writeback\n");
    return 1;
  }
  printf("[PASS] mapshared_lazy\n");
  return 0;
}
