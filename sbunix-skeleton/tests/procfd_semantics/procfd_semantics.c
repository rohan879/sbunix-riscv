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

static int same_file_stat(const struct stat *a, const struct stat *b) {
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
         ((a->st_mode & S_IFMT) == (b->st_mode & S_IFMT));
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

static void test_regular_reopen_preserves_identity(const char *path) {
  char procpath[80];
  char c = 0;
  struct stat st_src;
  struct stat st_reopen;
  struct stat st_replacement;

  unlink(path);
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd < 0) {
    check("procfd_semantics: identity setup", 0);
    return;
  }
  check("procfd_semantics: identity write old", write(fd, "old", 3) == 3);
  snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", fd);
  check("procfd_semantics: identity source stat", fstat(fd, &st_src) == 0);

  check("procfd_semantics: identity unlink source", unlink(path) == 0);
  int replacement = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  check("procfd_semantics: identity replacement create", replacement >= 0);
  if (replacement >= 0) {
    check("procfd_semantics: identity write replacement",
          write(replacement, "new", 3) == 3);
    check("procfd_semantics: identity replacement stat",
          fstat(replacement, &st_replacement) == 0);
    close(replacement);
  } else {
    memset(&st_replacement, 0, sizeof(st_replacement));
  }

  int rfd = open(procpath, O_RDONLY);
  check("procfd_semantics: identity procfd reopen succeeds", rfd >= 0);
  if (rfd >= 0) {
    check("procfd_semantics: identity procfd reads original",
          read_one(rfd, &c) == 1 && c == 'o');
    check("procfd_semantics: identity procfd stat matches source",
          fstat(rfd, &st_reopen) == 0 && same_file_stat(&st_src, &st_reopen));
    if (replacement >= 0) {
      check("procfd_semantics: identity differs from replacement",
            !same_file_stat(&st_reopen, &st_replacement));
    }
    close(rfd);
  }

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

  errno = 0;
  bad = open(procpath, O_CREAT | O_EXCL | O_RDONLY, 0644);
  check("procfd_semantics: pipe excl create reports EEXIST",
        bad < 0 && errno == EEXIST);
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

static void test_directory_reopen_flags(void) {
  int dirfd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY);
  check("procfd_semantics: directory procfd setup", dirfd >= 0);
  if (dirfd < 0)
    return;

  char procpath[64];
  snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", dirfd);
  errno = 0;
  int bad = open(procpath, O_RDONLY | O_TRUNC);
  check("procfd_semantics: directory procfd trunc reports EISDIR",
        bad < 0 && errno == EISDIR);
  if (bad >= 0)
    close(bad);
  close(dirfd);
}

static void test_procfd_excl_existing_backends(void) {
  const char *paths[] = { "/bin/cat", "/dev/null" };

  for (int i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++) {
    int fd = open(paths[i], O_RDONLY);
    check("procfd_semantics: excl backend setup", fd >= 0);
    if (fd < 0)
      continue;

    char procpath[64];
    snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", fd);
    errno = 0;
    int bad = open(procpath, O_RDONLY | O_CREAT | O_EXCL, 0644);
    check("procfd_semantics: excl existing procfd reports EEXIST",
          bad < 0 && errno == EEXIST);
    if (bad >= 0)
      close(bad);
    close(fd);
  }
}

static void test_devfs_procfd_preserves_state(void) {
  int master = open("/dev/ptmx", O_RDWR);
  int slave = open("/dev/pts/0", O_RDWR);
  check("procfd_semantics: devfs pty setup", master >= 0 && slave >= 0);
  if (master < 0 || slave < 0) {
    if (master >= 0)
      close(master);
    if (slave >= 0)
      close(slave);
    return;
  }

  char procpath[64];
  snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", master);
  check("procfd_semantics: devfs pty pending write", write(master, "x", 1) == 1);
  int reopened = open(procpath, O_RDWR);
  check("procfd_semantics: devfs pty procfd reopen", reopened >= 0);

  char c = 0;
  check("procfd_semantics: devfs pty buffer preserved",
        read_one(slave, &c) == 1 && c == 'x');
  if (reopened >= 0) {
    c = 0;
    check("procfd_semantics: devfs pty reopened writes same slave",
          write(reopened, "y", 1) == 1 &&
          read_one(slave, &c) == 1 && c == 'y');
    close(reopened);
  }
  close(slave);
  close(master);
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
  test_regular_reopen_preserves_identity("/tmp/procfd-sem-identity");
  test_regular_reopen_preserves_identity("/mnt/procfd-sem-identity");
  test_pipe_reopen();
  test_directory_reopen_flags();
  test_procfd_excl_existing_backends();
  test_devfs_procfd_preserves_state();
  test_common_proc_nodes();
  printf("=== procfd_semantics: %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
