#include <errno.h>
#include <fcntl.h>
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
  if (fd < 0)
    return -1;
  int len = strlen(data);
  int n = write(fd, data, len);
  close(fd);
  return n == len ? 0 : -1;
}

static int read_file(const char *path, char *buf, int bufsz) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  int total = 0;
  while (total < bufsz - 1) {
    int n = read(fd, buf + total, bufsz - 1 - total);
    if (n < 0) {
      close(fd);
      return -1;
    }
    if (n == 0)
      break;
    total += n;
  }
  close(fd);
  buf[total] = '\0';
  return total;
}

static void cleanup(void) {
  unlink("/tmp/pdse_linkdir");
  unlink("/tmp/pdse_filelink");
  unlink("/tmp/pdse_file");
  unlink("/tmp/pdse_sibling");
  unlink("/tmp/pdse_target/dir/inside");
  unlink("/tmp/pdse_target/sibling");
  rmdir("/tmp/pdse_target/dir");
  rmdir("/tmp/pdse_target");
  unlink("/tmp/pdse_base/linkrel");
  unlink("/tmp/pdse_base/target/dir/inside");
  unlink("/tmp/pdse_base/target/sibling");
  rmdir("/tmp/pdse_base/target/dir");
  rmdir("/tmp/pdse_base/target");
  rmdir("/tmp/pdse_base");
}

static void test_absolute_dir_symlink_dotdot(void) {
  char buf[64];

  check("path_dotdot_symlink_edges: absolute target dir setup",
        mkdir("/tmp/pdse_target", 0755) == 0 &&
        mkdir("/tmp/pdse_target/dir", 0755) == 0 &&
        write_file("/tmp/pdse_target/sibling", "target") == 0 &&
        write_file("/tmp/pdse_sibling", "wrong") == 0 &&
        symlink("/tmp/pdse_target/dir", "/tmp/pdse_linkdir") == 0);

  int n = read_file("/tmp/pdse_linkdir/../sibling", buf, sizeof(buf));
  check("path_dotdot_symlink_edges: absolute symlink before dotdot",
        n == 6 && strcmp(buf, "target") == 0);
}

static void test_relative_dir_symlink_dotdot(void) {
  char buf[64];

  check("path_dotdot_symlink_edges: relative target dir setup",
        mkdir("/tmp/pdse_base", 0755) == 0 &&
        mkdir("/tmp/pdse_base/target", 0755) == 0 &&
        mkdir("/tmp/pdse_base/target/dir", 0755) == 0 &&
        write_file("/tmp/pdse_base/target/sibling", "relative") == 0 &&
        symlink("target/dir", "/tmp/pdse_base/linkrel") == 0);

  int n = read_file("/tmp/pdse_base/linkrel/../sibling", buf, sizeof(buf));
  check("path_dotdot_symlink_edges: relative symlink before dotdot",
        n == 8 && strcmp(buf, "relative") == 0);
}

static void test_file_symlink_dotdot_enotdir(void) {
  struct stat st;

  check("path_dotdot_symlink_edges: file link setup",
        write_file("/tmp/pdse_file", "file") == 0 &&
        symlink("/tmp/pdse_file", "/tmp/pdse_filelink") == 0);

  errno = 0;
  expect_errno("path_dotdot_symlink_edges: file symlink dotdot ENOTDIR",
               stat("/tmp/pdse_filelink/..", &st), ENOTDIR);
}

int main(void) {
  printf("=== path_dotdot_symlink_edges ===\n");
  cleanup();

  test_absolute_dir_symlink_dotdot();
  test_relative_dir_symlink_dotdot();
  test_file_symlink_dotdot_enotdir();

  cleanup();
  printf("=== path_dotdot_symlink_edges: %d passed, %d failed ===\n",
         g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
