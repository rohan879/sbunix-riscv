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

static int read_one(int fd, char *out) {
  char c = 0;
  int n = read(fd, &c, 1);
  if (n == 1)
    *out = c;
  return n;
}

static void test_regular_reopen_is_fresh(void) {
  const char *path = "/tmp/procfd-sem-regular";
  char procpath[64];
  char c = 0;

  unlink(path);
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd < 0) {
    check("procfd_semantics: regular setup", 0);
    return;
  }
  check("procfd_semantics: regular write", write(fd, "abcd", 4) == 4);
  check("procfd_semantics: regular seek source", lseek(fd, 2, SEEK_SET) == 2);

  snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", fd);
  int rfd = open(procpath, O_RDONLY);
  check("procfd_semantics: regular procfd reopen succeeds", rfd >= 0);
  if (rfd >= 0) {
    check("procfd_semantics: regular procfd fresh offset",
          read_one(rfd, &c) == 1 && c == 'a');
    check("procfd_semantics: regular source offset unchanged",
          lseek(fd, 0, SEEK_CUR) == 2);
    close(rfd);
  }

  errno = 0;
  rfd = open(procpath, O_RDONLY | O_NOFOLLOW);
  check("procfd_semantics: regular nofollow reports ELOOP",
        rfd < 0 && errno == ELOOP);
  if (rfd >= 0)
    close(rfd);

  errno = 0;
  int dirfd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY);
  rfd = dirfd >= 0 ? openat(dirfd, procpath + strlen("/proc/self/fd/"),
                            O_RDONLY)
                   : -1;
  check("procfd_semantics: dirfd-relative procfd reopen",
        dirfd >= 0 && rfd >= 0);
  if (rfd >= 0) {
    check("procfd_semantics: dirfd-relative fresh offset",
          read_one(rfd, &c) == 1 && c == 'a');
    close(rfd);
  }
  if (dirfd >= 0)
    close(dirfd);

  close(fd);
  unlink(path);
}

static void test_pipe_reopen(void) {
  int pfd[2];
  char procpath[64];
  char c = 0;

  if (pipe(pfd) != 0) {
    check("procfd_semantics: pipe setup", 0);
    return;
  }

  snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", pfd[0]);
  int wfd = open(procpath, O_WRONLY);
  check("procfd_semantics: pipe read-end reopened writable", wfd >= 0);
  if (wfd >= 0) {
    check("procfd_semantics: pipe procfd write", write(wfd, "z", 1) == 1);
    check("procfd_semantics: pipe procfd data visible",
          read_one(pfd[0], &c) == 1 && c == 'z');
    close(wfd);
  }

  errno = 0;
  int bad = open(procpath, O_RDONLY | O_NOFOLLOW);
  check("procfd_semantics: pipe nofollow reports ELOOP",
        bad < 0 && errno == ELOOP);
  if (bad >= 0)
    close(bad);

  char slashpath[80];
  snprintf(slashpath, sizeof(slashpath), "%s/", procpath);
  errno = 0;
  bad = open(slashpath, O_RDONLY);
  check("procfd_semantics: pipe trailing slash reports ENOTDIR",
        bad < 0 && errno == ENOTDIR);
  if (bad >= 0)
    close(bad);

  close(pfd[0]);
  close(pfd[1]);
}

static void test_common_proc_nodes(void) {
  const char *nodes[] = {
    "/proc/self",
    "/proc/self/status",
    "/proc/self/stat",
    "/proc/self/fd",
    "/proc/self/maps",
    "/proc/self/exe",
    "/proc/self/cwd",
    "/proc/meminfo",
    "/proc/mounts",
    "/proc/cpuinfo",
    "/proc/version",
    "/proc/uptime",
  };
  struct stat st;

  for (int i = 0; i < (int)(sizeof(nodes) / sizeof(nodes[0])); i++) {
    errno = 0;
    check(nodes[i], lstat(nodes[i], &st) == 0);
  }
}

int main(void) {
  printf("=== procfd_semantics ===\n");
  test_regular_reopen_is_fresh();
  test_pipe_reopen();
  test_common_proc_nodes();
  printf("=== procfd_semantics: %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
