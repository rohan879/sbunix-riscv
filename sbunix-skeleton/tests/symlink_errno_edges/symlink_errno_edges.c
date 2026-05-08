#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_pass;
static int g_fail;

static void check(const char *name, int ok) {
  if (ok) {
    g_pass++;
    printf("[PASS] %s\n", name);
  } else {
    g_fail++;
    printf("[FAIL] %s errno=%d\n", name, errno);
  }
}

static void expect_errno(const char *name, int result, int want_errno) {
  check(name, result < 0 && errno == want_errno);
}

static int write_file(const char *path, const char *data) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  int len = strlen(data);
  if (fd < 0)
    return -1;
  if (write(fd, data, len) != len) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static void fill_chars(char *buf, int len, char ch) {
  for (int i = 0; i < len; i++)
    buf[i] = ch;
  buf[len] = '\0';
}

static void cleanup(void) {
  unlink("/tmp/see_exist");
  unlink("/tmp/see_file");
  unlink("/tmp/see_loop_a");
  unlink("/tmp/see_loop_b");
  unlink("/tmp/see_dang");
  unlink("/tmp/see_empty_target");
}

int main(void) {
  char buf[16];
  char long_target[257];
  char long_linkpath[400];
  int fd;

  printf("=== symlink_errno_edges ===\n");
  cleanup();

  errno = 0;
  expect_errno("symlink_errno_edges: empty linkpath ENOENT",
               symlinkat("x", AT_FDCWD, ""), ENOENT);

  errno = 0;
  expect_errno("symlink_errno_edges: bad newdirfd EBADF",
               symlinkat("x", 99, "see_badfd"), EBADF);

  check("symlink_errno_edges: nondir newdirfd setup",
        write_file("/tmp/see_file", "x") == 0);
  fd = open("/tmp/see_file", O_RDONLY);
  check("symlink_errno_edges: nondir newdirfd open", fd >= 0);
  if (fd >= 0) {
    errno = 0;
    expect_errno("symlink_errno_edges: nondir newdirfd ENOTDIR",
                 symlinkat("x", fd, "child"), ENOTDIR);
    close(fd);
  }

  check("symlink_errno_edges: existing destination setup",
        write_file("/tmp/see_exist", "x") == 0);
  errno = 0;
  expect_errno("symlink_errno_edges: existing destination EEXIST",
               symlinkat("x", AT_FDCWD, "/tmp/see_exist"), EEXIST);

  errno = 0;
  expect_errno("symlink_errno_edges: missing parent ENOENT",
               symlinkat("x", AT_FDCWD, "/tmp/see_missing_parent/child"),
               ENOENT);

  check("symlink_errno_edges: dangling parent symlink setup",
        symlink("/tmp/see_missing_real", "/tmp/see_dang") == 0);
  errno = 0;
  expect_errno("symlink_errno_edges: dangling parent symlink ENOENT",
               symlinkat("x", AT_FDCWD, "/tmp/see_dang/child"), ENOENT);

  check("symlink_errno_edges: parent loop setup",
        symlink("/tmp/see_loop_b", "/tmp/see_loop_a") == 0 &&
        symlink("/tmp/see_loop_a", "/tmp/see_loop_b") == 0);
  errno = 0;
  expect_errno("symlink_errno_edges: parent loop ELOOP",
               symlinkat("x", AT_FDCWD, "/tmp/see_loop_a/child"), ELOOP);

  errno = 0;
  expect_errno("symlink_errno_edges: readonly mount EROFS",
               symlinkat("x", AT_FDCWD, "/bin/see_ro_link"), EROFS);

  fill_chars(long_target, 256, 't');
  errno = 0;
  expect_errno("symlink_errno_edges: long target ENAMETOOLONG",
               symlinkat(long_target, AT_FDCWD, "/tmp/see_long_target"),
               ENAMETOOLONG);

  strcpy(long_linkpath, "/tmp/");
  fill_chars(long_linkpath + 5, (int)sizeof(long_linkpath) - 6, 'p');
  errno = 0;
  expect_errno("symlink_errno_edges: long linkpath ENAMETOOLONG",
               symlinkat("x", AT_FDCWD, long_linkpath), ENAMETOOLONG);

  check("symlink_errno_edges: empty target create",
        symlinkat("", AT_FDCWD, "/tmp/see_empty_target") == 0);
  memset(buf, 'Q', sizeof(buf));
  check("symlink_errno_edges: empty target readlink zero",
        readlink("/tmp/see_empty_target", buf, sizeof(buf)) == 0 &&
        buf[0] == 'Q');

  cleanup();
  printf("=== symlink_errno_edges: %d passed, %d failed ===\n",
         g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
