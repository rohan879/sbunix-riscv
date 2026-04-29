#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_pass;
static int g_fail;

#define CHECK(label, cond) do { \
  if (cond) { printf("[PASS] ttyname_edges: %s\n", label); g_pass++; } \
  else { printf("[FAIL] ttyname_edges: %s\n", label); g_fail++; } \
} while (0)

static void test_isatty_errors(void) {
  errno = 0;
  CHECK("isatty invalid fd reports EBADF",
        isatty(-1) == 0 && errno == EBADF);

  int fd = open("/tmp/ttyname_edges_regular", O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd < 0) {
    CHECK("regular file setup", 0);
    return;
  }
  errno = 0;
  CHECK("isatty regular file reports ENOTTY",
        isatty(fd) == 0 && errno == ENOTTY);
  close(fd);
  unlink("/tmp/ttyname_edges_regular");
}

static void test_ttyname_r_errors(void) {
  char buf[64];
  int fd = open("/tmp/ttyname_edges_regular", O_CREAT | O_RDWR | O_TRUNC, 0644);

  CHECK("ttyname_r NULL buffer", ttyname_r(0, NULL, sizeof(buf)) == EINVAL);
  CHECK("ttyname_r zero buffer", ttyname_r(0, buf, 0) == ERANGE);
  CHECK("ttyname_r invalid fd", ttyname_r(-1, buf, sizeof(buf)) == EBADF);

  if (fd >= 0) {
    CHECK("ttyname_r regular file", ttyname_r(fd, buf, sizeof(buf)) == ENOTTY);
    close(fd);
    unlink("/tmp/ttyname_edges_regular");
  } else {
    CHECK("ttyname_r regular file", 0);
  }
}

static void test_ttyname_path(void) {
  char buf[64];
  char small[2];
  char link[4];
  int n;

  CHECK("stdin is a tty", isatty(STDIN_FILENO) == 1);
  CHECK("ttyname_r stdin succeeds", ttyname_r(STDIN_FILENO, buf, sizeof(buf)) == 0);
  if (buf[0])
    CHECK("ttyname_r stdin reports dev path", strncmp(buf, "/dev/", 5) == 0);
  else
    CHECK("ttyname_r stdin reports dev path", 0);

  CHECK("ttyname_r small buffer gets ERANGE",
        ttyname_r(STDIN_FILENO, small, sizeof(small)) == ERANGE);

  n = readlink("/proc/self/fd/0", link, sizeof(link));
  CHECK("proc fd readlink truncates without failing", n == (int)sizeof(link));
}

int main(void) {
  test_isatty_errors();
  test_ttyname_r_errors();
  test_ttyname_path();

  printf("=== ttyname_edges: %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
