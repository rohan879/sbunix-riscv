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

static void fill_target(char *buf, int len) {
  for (int i = 0; i < len; i++)
    buf[i] = (char)('a' + (i % 26));
  buf[len] = '\0';
}

static void cleanup(void) {
  unlink("/tmp/slim_max");
  unlink("/tmp/slim_too_long");
  unlink("/tmp/slim_existing");
  unlink("/tmp/slim_bad\nname");
}

int main(void) {
  char t255[256];
  char t256[257];
  char buf[300];
  struct stat st;
  int n;

  printf("=== symlink_limits_edges ===\n");
  cleanup();
  fill_target(t255, 255);
  fill_target(t256, 256);

  check("symlink_limits_edges: 255-byte target succeeds",
        symlink(t255, "/tmp/slim_max") == 0);
  n = readlink("/tmp/slim_max", buf, sizeof(buf));
  check("symlink_limits_edges: 255-byte readlink complete",
        n == 255 && memcmp(buf, t255, 255) == 0);

  errno = 0;
  check("symlink_limits_edges: 256-byte target ENAMETOOLONG",
        symlink(t256, "/tmp/slim_too_long") < 0 &&
        errno == ENAMETOOLONG);
  check("symlink_limits_edges: failed long create leaves no node",
        lstat("/tmp/slim_too_long", &st) < 0 && errno == ENOENT);

  check("symlink_limits_edges: existing setup",
        symlink("old", "/tmp/slim_existing") == 0);
  errno = 0;
  check("symlink_limits_edges: existing beats replacement",
        symlink(t256, "/tmp/slim_existing") < 0 && errno == EEXIST);
  n = readlink("/tmp/slim_existing", buf, sizeof(buf));
  check("symlink_limits_edges: existing destination untouched",
        n == 3 && memcmp(buf, "old", 3) == 0);

  errno = 0;
  check("symlink_limits_edges: newline basename rejected",
        symlink("target", "/tmp/slim_bad\nname") < 0 &&
        errno == EILSEQ);
  check("symlink_limits_edges: newline failure leaves no node",
        lstat("/tmp/slim_bad\nname", &st) < 0 && errno == ENOENT);

  cleanup();
  printf("=== symlink_limits_edges: %d passed, %d failed ===\n",
         g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
