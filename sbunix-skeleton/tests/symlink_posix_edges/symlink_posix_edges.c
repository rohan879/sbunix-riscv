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

static void cleanup(void) {
  unlink("/tmp/spos_empty");
  unlink("/tmp/spos_existing");
  unlink("/tmp/spos_opaque");
  unlink("/tmp/spos_file");
}

int main(void) {
  char buf[128];
  int fd;
  int n;

  printf("=== symlink_posix_edges ===\n");
  cleanup();

  check("symlink_posix_edges: empty target create",
        symlink("", "/tmp/spos_empty") == 0);
  memset(buf, 'X', sizeof(buf));
  n = readlink("/tmp/spos_empty", buf, sizeof(buf));
  check("symlink_posix_edges: empty target readlink zero", n == 0);
  check("symlink_posix_edges: empty target no implicit NUL", buf[0] == 'X');

  check("symlink_posix_edges: opaque missing target create",
        symlink("../missing/../opaque-target", "/tmp/spos_opaque") == 0);
  n = readlink("/tmp/spos_opaque", buf, sizeof(buf) - 1);
  if (n >= 0)
    buf[n] = '\0';
  check("symlink_posix_edges: opaque target text preserved",
        n > 0 && strcmp(buf, "../missing/../opaque-target") == 0);

  check("symlink_posix_edges: existing setup",
        symlink("old-target", "/tmp/spos_existing") == 0);
  errno = 0;
  check("symlink_posix_edges: existing path returns EEXIST",
        symlink("new-target", "/tmp/spos_existing") < 0 &&
        errno == EEXIST);
  n = readlink("/tmp/spos_existing", buf, sizeof(buf) - 1);
  if (n >= 0)
    buf[n] = '\0';
  check("symlink_posix_edges: existing link left untouched",
        n > 0 && strcmp(buf, "old-target") == 0);

  errno = 0;
  check("symlink_posix_edges: symlinkat bad fd EBADF",
        symlinkat("target", 99, "spos_badfd") < 0 && errno == EBADF);

  fd = open("/tmp/spos_file", O_CREAT | O_RDWR | O_TRUNC, 0644);
  check("symlink_posix_edges: non-dir fd setup", fd >= 0);
  if (fd >= 0) {
    errno = 0;
    check("symlink_posix_edges: symlinkat non-dir fd ENOTDIR",
          symlinkat("target", fd, "child") < 0 && errno == ENOTDIR);
    close(fd);
  }

  cleanup();
  printf("=== symlink_posix_edges: %d passed, %d failed ===\n",
         g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
