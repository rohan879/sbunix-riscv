#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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
    if (n <= 0)
      break;
    total += n;
  }
  close(fd);
  buf[total] = '\0';
  return total;
}

static void cleanup_chain(const char *prefix, int max_idx) {
  char path[64];

  for (int i = 0; i <= max_idx; i++) {
    snprintf(path, sizeof(path), "/tmp/%s%d", prefix, i);
    unlink(path);
  }
}

static void cleanup_all(void) {
  unlink("/tmp/sme_link");
  unlink("/tmp/sme_renamed");
  unlink("/tmp/sme_hard_link");
  unlink("/tmp/sme_hard_follow");
  unlink("/tmp/sme_target");
  unlink("/tmp/sme_filelink");
  unlink("/tmp/sme_file");
  unlink("/tmp/sme_dirlink");
  unlink("/tmp/sme_dir/child");
  unlink("/tmp/sme_dir/missing-relative");
  rmdir("/tmp/sme_dir");
  unlink("/tmp/sme_parentlink");
  unlink("/tmp/sme_parent/child");
  unlink("/tmp/sme_parent/missing-relative");
  rmdir("/tmp/sme_parent");
  cleanup_chain("sme40_", 41);
  cleanup_chain("sme41_", 42);
}

static void test_link_rename_unlink_symlink_itself(void) {
  struct stat st;
  char buf[64];

  unlink("/tmp/sme_link");
  unlink("/tmp/sme_renamed");
  unlink("/tmp/sme_hard_link");
  unlink("/tmp/sme_hard_follow");
  unlink("/tmp/sme_target");

  check("symlink_mutation_edges: target setup",
        write_file("/tmp/sme_target", "target") == 0);
  check("symlink_mutation_edges: link setup",
        symlink("/tmp/sme_target", "/tmp/sme_link") == 0);

  check("symlink_mutation_edges: linkat preserves link",
        linkat(AT_FDCWD, "/tmp/sme_link", AT_FDCWD,
               "/tmp/sme_hard_link", 0) == 0);
  check("symlink_mutation_edges: hard link is symlink",
        lstat("/tmp/sme_hard_link", &st) == 0 && S_ISLNK(st.st_mode));
  int n = readlink("/tmp/sme_hard_link", buf, sizeof(buf) - 1);
  if (n > 0)
    buf[n] = '\0';
  check("symlink_mutation_edges: hard link target text",
        n > 0 && strcmp(buf, "/tmp/sme_target") == 0);

  check("symlink_mutation_edges: linkat follows with flag",
        linkat(AT_FDCWD, "/tmp/sme_link", AT_FDCWD,
               "/tmp/sme_hard_follow", AT_SYMLINK_FOLLOW) == 0);
  check("symlink_mutation_edges: followed hard link regular",
        lstat("/tmp/sme_hard_follow", &st) == 0 && S_ISREG(st.st_mode));
  n = read_file("/tmp/sme_hard_follow", buf, sizeof(buf));
  check("symlink_mutation_edges: followed hard link content",
        n == 6 && strcmp(buf, "target") == 0);

  check("symlink_mutation_edges: renameat moves link itself",
        renameat(AT_FDCWD, "/tmp/sme_link", AT_FDCWD,
                 "/tmp/sme_renamed") == 0);
  check("symlink_mutation_edges: renamed object is link",
        lstat("/tmp/sme_renamed", &st) == 0 && S_ISLNK(st.st_mode));
  n = read_file("/tmp/sme_renamed", buf, sizeof(buf));
  check("symlink_mutation_edges: renamed link still resolves",
        n == 6 && strcmp(buf, "target") == 0);

  check("symlink_mutation_edges: unlinkat removes link",
        unlinkat(AT_FDCWD, "/tmp/sme_renamed", 0) == 0);
  check("symlink_mutation_edges: unlinkat target survives",
        read_file("/tmp/sme_target", buf, sizeof(buf)) == 6 &&
        strcmp(buf, "target") == 0);

  unlink("/tmp/sme_hard_link");
  unlink("/tmp/sme_hard_follow");
  unlink("/tmp/sme_target");
}

static void test_trailing_slash_directory_target(void) {
  struct stat st;
  struct timespec ts[2];

  unlink("/tmp/sme_dirlink");
  unlink("/tmp/sme_filelink");
  unlink("/tmp/sme_file");
  rmdir("/tmp/sme_dir");

  check("symlink_mutation_edges: directory setup",
        mkdir("/tmp/sme_dir", 0755) == 0);
  check("symlink_mutation_edges: file setup",
        write_file("/tmp/sme_file", "f") == 0);
  check("symlink_mutation_edges: dir symlink setup",
        symlink("/tmp/sme_dir", "/tmp/sme_dirlink") == 0);
  check("symlink_mutation_edges: file symlink setup",
        symlink("/tmp/sme_file", "/tmp/sme_filelink") == 0);

  int fd = open("/tmp/sme_dirlink/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  check("symlink_mutation_edges: open slash follows dir link", fd >= 0);
  if (fd >= 0)
    close(fd);

  errno = 0;
  expect_errno("symlink_mutation_edges: open no slash nofollow ELOOP",
               open("/tmp/sme_dirlink", O_RDONLY | O_NOFOLLOW), ELOOP);

  check("symlink_mutation_edges: fchownat slash follows dir link",
        fchownat(AT_FDCWD, "/tmp/sme_dirlink/", 0, 0,
                 AT_SYMLINK_NOFOLLOW) == 0);
  ts[0].tv_sec = 123;
  ts[0].tv_nsec = 0;
  ts[1].tv_sec = 456;
  ts[1].tv_nsec = 0;
  check("symlink_mutation_edges: utimensat slash follows dir link",
        utimensat(AT_FDCWD, "/tmp/sme_dirlink/", ts,
                  AT_SYMLINK_NOFOLLOW) == 0);
  check("symlink_mutation_edges: directory mtime changed",
        stat("/tmp/sme_dir", &st) == 0 && st.st_mtime == 456);

  errno = 0;
  expect_errno("symlink_mutation_edges: unlinkat slash dir link ENOTDIR",
               unlinkat(AT_FDCWD, "/tmp/sme_dirlink/", 0), ENOTDIR);
  errno = 0;
  expect_errno("symlink_mutation_edges: rmdir slash dir link ENOTDIR",
               unlinkat(AT_FDCWD, "/tmp/sme_dirlink/", AT_REMOVEDIR),
               ENOTDIR);
  errno = 0;
  expect_errno("symlink_mutation_edges: linkat old slash dir link EPERM",
               linkat(AT_FDCWD, "/tmp/sme_dirlink/", AT_FDCWD,
                      "/tmp/sme_dir_hard", 0), EPERM);
  unlink("/tmp/sme_dir_hard");
  errno = 0;
  expect_errno("symlink_mutation_edges: linkat new slash dir link EEXIST",
               linkat(AT_FDCWD, "/tmp/sme_file", AT_FDCWD,
                      "/tmp/sme_dirlink/", 0), EEXIST);

  errno = 0;
  expect_errno("symlink_mutation_edges: fchownat slash file link ENOTDIR",
               fchownat(AT_FDCWD, "/tmp/sme_filelink/", 0, 0,
                        AT_SYMLINK_NOFOLLOW), ENOTDIR);
  errno = 0;
  expect_errno("symlink_mutation_edges: utimensat slash file link ENOTDIR",
               utimensat(AT_FDCWD, "/tmp/sme_filelink/", ts,
                         AT_SYMLINK_NOFOLLOW), ENOTDIR);

  errno = 0;
  expect_errno("symlink_mutation_edges: mkdirat existing dir link EEXIST",
               mkdirat(AT_FDCWD, "/tmp/sme_dirlink/", 0755), EEXIST);
  errno = 0;
  expect_errno("symlink_mutation_edges: mknodat existing dir link EEXIST",
               mknodat(AT_FDCWD, "/tmp/sme_dirlink/",
                       S_IFCHR | 0600, 0), EEXIST);
  errno = 0;
  expect_errno("symlink_mutation_edges: symlinkat existing dir link EEXIST",
               symlinkat("x", AT_FDCWD, "/tmp/sme_dirlink/"), EEXIST);

  unlink("/tmp/sme_filelink");
  unlink("/tmp/sme_file");
  unlink("/tmp/sme_dirlink");
  rmdir("/tmp/sme_dir");
}

static void test_relative_dangling_link_flags(void) {
  char buf[64];
  struct timespec ts[2];

  unlink("/tmp/sme_parentlink");
  unlink("/tmp/sme_parent/child");
  unlink("/tmp/sme_parent/missing-relative");
  rmdir("/tmp/sme_parent");

  check("symlink_mutation_edges: parent mkdir",
        mkdir("/tmp/sme_parent", 0755) == 0);
  check("symlink_mutation_edges: parent link",
        symlink("/tmp/sme_parent", "/tmp/sme_parentlink") == 0);
  check("symlink_mutation_edges: symlinkat through parent link",
        symlinkat("missing-relative", AT_FDCWD,
                  "/tmp/sme_parentlink/child") == 0);

  memset(buf, 0, sizeof(buf));
  int n = readlink("/tmp/sme_parent/child", buf, sizeof(buf) - 1);
  if (n > 0)
    buf[n] = '\0';
  check("symlink_mutation_edges: created relative link text",
        n > 0 && strcmp(buf, "missing-relative") == 0);

  errno = 0;
  expect_errno("symlink_mutation_edges: dangling relative open ENOENT",
               open("/tmp/sme_parent/child", O_RDONLY), ENOENT);

  errno = 0;
  expect_errno("symlink_mutation_edges: fchownat follows dangling ENOENT",
               fchownat(AT_FDCWD, "/tmp/sme_parent/child", 0, 0, 0),
               ENOENT);
  check("symlink_mutation_edges: fchownat nofollow dangling link",
        fchownat(AT_FDCWD, "/tmp/sme_parent/child", 0, 0,
                 AT_SYMLINK_NOFOLLOW) == 0);

  ts[0].tv_sec = 321;
  ts[0].tv_nsec = 0;
  ts[1].tv_sec = 654;
  ts[1].tv_nsec = 0;
  errno = 0;
  expect_errno("symlink_mutation_edges: utimensat follows dangling ENOENT",
               utimensat(AT_FDCWD, "/tmp/sme_parent/child", ts, 0),
               ENOENT);
  check("symlink_mutation_edges: utimensat nofollow dangling link",
        utimensat(AT_FDCWD, "/tmp/sme_parent/child", ts,
                  AT_SYMLINK_NOFOLLOW) == 0);

  check("symlink_mutation_edges: create relative target",
        write_file("/tmp/sme_parent/missing-relative", "rel") == 0);
  n = read_file("/tmp/sme_parent/child", buf, sizeof(buf));
  check("symlink_mutation_edges: relative link resolves after target",
        n == 3 && strcmp(buf, "rel") == 0);

  unlink("/tmp/sme_parent/child");
  unlink("/tmp/sme_parent/missing-relative");
  unlink("/tmp/sme_parentlink");
  rmdir("/tmp/sme_parent");
}

static void chain_path(char *buf, int bufsz, const char *prefix, int idx) {
  snprintf(buf, bufsz, "/tmp/%s%d", prefix, idx);
}

static void test_hop_limits(void) {
  char cur[64];
  char next[64];
  char buf[16];

  cleanup_chain("sme40_", 41);
  cleanup_chain("sme41_", 42);

  chain_path(cur, sizeof(cur), "sme40_", 40);
  check("symlink_mutation_edges: 40-hop target setup",
        write_file(cur, "ok") == 0);
  for (int i = 39; i >= 0; i--) {
    chain_path(cur, sizeof(cur), "sme40_", i);
    chain_path(next, sizeof(next), "sme40_", i + 1);
    check("symlink_mutation_edges: 40-hop link setup",
          symlink(next, cur) == 0);
  }
  chain_path(cur, sizeof(cur), "sme40_", 0);
  int n = read_file(cur, buf, sizeof(buf));
  check("symlink_mutation_edges: 40-hop chain resolves",
        n == 2 && strcmp(buf, "ok") == 0);
  cleanup_chain("sme40_", 41);

  chain_path(cur, sizeof(cur), "sme41_", 41);
  check("symlink_mutation_edges: 41-hop target setup",
        write_file(cur, "bad") == 0);
  for (int i = 40; i >= 0; i--) {
    chain_path(cur, sizeof(cur), "sme41_", i);
    chain_path(next, sizeof(next), "sme41_", i + 1);
    check("symlink_mutation_edges: 41-hop link setup",
          symlink(next, cur) == 0);
  }
  chain_path(cur, sizeof(cur), "sme41_", 0);
  errno = 0;
  int fd = open(cur, O_RDONLY);
  expect_errno("symlink_mutation_edges: 41-hop chain ELOOP", fd, ELOOP);
  if (fd >= 0)
    close(fd);

  cleanup_chain("sme40_", 41);
  cleanup_chain("sme41_", 42);
}

int main(void) {
  printf("=== symlink_mutation_edges ===\n");

  cleanup_all();
  test_link_rename_unlink_symlink_itself();
  test_trailing_slash_directory_target();
  test_relative_dangling_link_flags();
  test_hop_limits();
  cleanup_all();

  printf("=== symlink_mutation_edges: %d passed, %d failed ===\n",
         g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
