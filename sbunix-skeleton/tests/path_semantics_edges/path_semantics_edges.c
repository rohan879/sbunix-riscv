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

static void cleanup(void) {
  unlink("/tmp/pathsem-linkdir");
  unlink("/tmp/pathsem-file");
  unlink("/tmp/pathsem-created");
  rmdir("/tmp/pathsem-dir");
}

static int setup(void) {
  cleanup();
  int fd = open("/tmp/pathsem-file", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0)
    return -1;
  if (write(fd, "x", 1) != 1) {
    close(fd);
    return -1;
  }
  close(fd);
  if (mkdir("/tmp/pathsem-dir", 0755) != 0)
    return -1;
  if (symlink("/tmp/pathsem-dir", "/tmp/pathsem-linkdir") != 0)
    return -1;
  return 0;
}

static void test_trailing_slash_errors(void) {
  struct stat st;
  char buf[32];

  errno = 0;
  int fd = open("/tmp/pathsem-file/", O_RDONLY);
  expect_errno("path_semantics_edges: open regular with slash ENOTDIR",
               fd, ENOTDIR);
  if (fd >= 0)
    close(fd);

  errno = 0;
  expect_errno("path_semantics_edges: stat regular with slash ENOTDIR",
               stat("/tmp/pathsem-file/", &st), ENOTDIR);

  errno = 0;
  expect_errno("path_semantics_edges: access regular with slash ENOTDIR",
               access("/tmp/pathsem-file/", F_OK), ENOTDIR);

  errno = 0;
  expect_errno("path_semantics_edges: readlink regular with slash ENOTDIR",
               (int)readlink("/tmp/pathsem-file/", buf, sizeof(buf)),
               ENOTDIR);

  errno = 0;
  fd = open("/tmp/pathsem-missing/", O_RDONLY);
  expect_errno("path_semantics_edges: open missing with slash ENOENT",
               fd, ENOENT);
  if (fd >= 0)
    close(fd);

  errno = 0;
  expect_errno("path_semantics_edges: mkdir regular with slash ENOTDIR",
               mkdir("/tmp/pathsem-file/", 0755), ENOTDIR);

  errno = 0;
  check("path_semantics_edges: mkdir missing with slash creates directory",
        mkdir("/tmp/pathsem-created/", 0755) == 0 &&
        stat("/tmp/pathsem-created", &st) == 0 && S_ISDIR(st.st_mode));
  rmdir("/tmp/pathsem-created");

  errno = 0;
  expect_errno("path_semantics_edges: unlink regular with slash ENOTDIR",
               unlink("/tmp/pathsem-file/"), ENOTDIR);
  check("path_semantics_edges: unlink slash did not remove file",
        stat("/tmp/pathsem-file", &st) == 0);
}

static void test_trailing_slash_follows_dir_symlink(void) {
  int fd;

  errno = 0;
  fd = open("/tmp/pathsem-linkdir/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  check("path_semantics_edges: trailing slash follows dir symlink",
        fd >= 0);
  if (fd >= 0)
    close(fd);

  errno = 0;
  fd = open("/tmp/pathsem-linkdir", O_RDONLY | O_NOFOLLOW);
  expect_errno("path_semantics_edges: nofollow without slash ELOOP",
               fd, ELOOP);
  if (fd >= 0)
    close(fd);
}

static void test_dirfd_relative_edges(void) {
  struct stat st;
  int dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
  int ffd = open("/tmp/pathsem-file", O_RDONLY);
  int pfd[2];

  check("path_semantics_edges: dirfd setup", dfd >= 0 && ffd >= 0);

  errno = 0;
  int fd = openat(dfd, "pathsem-file/", O_RDONLY);
  expect_errno("path_semantics_edges: openat regular slash ENOTDIR",
               fd, ENOTDIR);
  if (fd >= 0)
    close(fd);

  errno = 0;
  expect_errno("path_semantics_edges: fstatat regular slash ENOTDIR",
               fstatat(dfd, "pathsem-file/", &st, 0), ENOTDIR);

  errno = 0;
  fd = openat(ffd, "child", O_RDONLY);
  expect_errno("path_semantics_edges: openat regular dirfd ENOTDIR",
               fd, ENOTDIR);
  if (fd >= 0)
    close(fd);

  errno = 0;
  fd = openat(9999, "child", O_RDONLY);
  expect_errno("path_semantics_edges: openat bad dirfd EBADF", fd, EBADF);
  if (fd >= 0)
    close(fd);

  if (pipe(pfd) == 0) {
    errno = 0;
    fd = openat(pfd[0], "child", O_RDONLY);
    expect_errno("path_semantics_edges: openat pipe dirfd ENOTDIR",
                 fd, ENOTDIR);
    if (fd >= 0)
      close(fd);
    close(pfd[0]);
    close(pfd[1]);
  } else {
    check("path_semantics_edges: pipe dirfd setup", 0);
  }

  if (ffd >= 0)
    close(ffd);
  if (dfd >= 0)
    close(dfd);
}

int main(void) {
  printf("=== path_semantics_edges ===\n");
  if (setup() != 0) {
    check("path_semantics_edges: setup", 0);
    return 1;
  }
  test_trailing_slash_errors();
  test_trailing_slash_follows_dir_symlink();
  test_dirfd_relative_edges();
  cleanup();
  printf("=== path_semantics_edges: %d passed, %d failed ===\n",
         g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
