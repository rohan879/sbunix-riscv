#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <syscall.h>
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

static int simple_atoi(const char *s) {
  int val = 0;
  while (*s >= '0' && *s <= '9') {
    val = val * 10 + (*s - '0');
    s++;
  }
  return val;
}

static int stat_fstat_match(const struct stat *a, const struct stat *b) {
  if (a->st_dev != b->st_dev || a->st_ino != b->st_ino)
    return 0;
  if ((a->st_mode & S_IFMT) != (b->st_mode & S_IFMT))
    return 0;
  return a->st_size == b->st_size;
}

static int stat_fstat_inode_match(const struct stat *a, const struct stat *b) {
  if (a->st_dev != b->st_dev || a->st_ino != b->st_ino)
    return 0;
  return (a->st_mode & S_IFMT) == (b->st_mode & S_IFMT);
}

static int read_all_fd(int fd, char *buf, int bufsz, int chunk) {
  int total = 0;

  while (total < bufsz) {
    int want = bufsz - total;
    int n;
    if (want > chunk)
      want = chunk;
    n = read(fd, buf + total, want);
    if (n < 0)
      return -1;
    if (n == 0)
      break;
    total += n;
  }
  return total;
}

static int count_lines(const char *buf, int len) {
  int lines = 0;
  for (int i = 0; i < len; i++)
    if (buf[i] == '\n')
      lines++;
  return lines;
}

static void test_stat_fstat_agreement(void) {
  const char *paths[] = {
    "/proc",
    "/proc/self",
    "/proc/self/status",
    "/proc/self/stat",
    "/proc/self/maps",
    "/proc/self/fd",
    "/proc/self/root",
    "/proc/meminfo",
    "/proc/mounts",
    "/proc/cpuinfo",
    "/proc/version",
    "/proc/loadavg",
  };
  const char *dynamic_paths[] = {
    "/proc/uptime",
  };
  int ok = 1;

  for (int i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++) {
    struct stat st_path;
    struct stat st_fd;
    int fd;

    errno = 0;
    if (stat(paths[i], &st_path) < 0) {
      ok = 0;
      continue;
    }
    fd = open(paths[i], O_RDONLY);
    if (fd < 0) {
      ok = 0;
      continue;
    }
    if (fstat(fd, &st_fd) < 0 || !stat_fstat_match(&st_path, &st_fd))
      ok = 0;
    close(fd);
  }

  for (int i = 0; i < (int)(sizeof(dynamic_paths) / sizeof(dynamic_paths[0])); i++) {
    struct stat st_path;
    struct stat st_fd;
    int fd;

    if (stat(dynamic_paths[i], &st_path) < 0) {
      ok = 0;
      continue;
    }
    fd = open(dynamic_paths[i], O_RDONLY);
    if (fd < 0) {
      ok = 0;
      continue;
    }
    if (fstat(fd, &st_fd) < 0 ||
        !stat_fstat_inode_match(&st_path, &st_fd))
      ok = 0;
    close(fd);
  }

  check("procfs_edges: stat and fstat metadata agree", ok);
}

static void test_readonly_rejections(void) {
  int fd;
  char c = 'x';

  fd = open("/proc/self/status", O_RDONLY);
  check("procfs_edges: O_RDONLY opens proc file", fd >= 0);
  if (fd >= 0) {
    errno = 0;
    check("procfs_edges: write on read fd rejected",
          write(fd, &c, 1) < 0 && errno == EBADF);
    errno = 0;
    check("procfs_edges: ftruncate on read fd rejected",
          ftruncate(fd, 0) < 0 && errno == EBADF);
    close(fd);
  }

  errno = 0;
  fd = open("/proc/self/status", O_WRONLY);
  check("procfs_edges: write-only proc open rejected",
        fd < 0 && errno == EACCES);
  if (fd >= 0)
    close(fd);

  errno = 0;
  fd = open("/proc/self/status", O_RDWR);
  check("procfs_edges: read-write proc open rejected",
        fd < 0 && errno == EACCES);
  if (fd >= 0)
    close(fd);

  errno = 0;
  fd = open("/proc/self/status", O_RDONLY | O_TRUNC);
  check("procfs_edges: truncating proc open rejected",
        fd < 0 && errno == EROFS);
  if (fd >= 0)
    close(fd);

  errno = 0;
  fd = open("/proc/procfs-edge-create", O_RDONLY | O_CREAT, 0644);
  check("procfs_edges: proc create rejected", fd < 0 && errno == EROFS);
  if (fd >= 0)
    close(fd);
}

static void test_maps_chunked_and_reseek(void) {
  enum { MAP_COUNT = 32 };
  void *maps[MAP_COUNT];
  char buf[8192];
  char first[17];
  char again[17];
  int made = 0;
  int fd;
  int total;
  int eof;

  for (int i = 0; i < MAP_COUNT; i++) {
    maps[i] = mmap(0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (maps[i] == MAP_FAILED)
      break;
    made++;
  }

  fd = open("/proc/self/maps", O_RDONLY);
  total = fd >= 0 ? read_all_fd(fd, buf, sizeof(buf), 11) : -1;
  eof = fd >= 0 ? read(fd, first, sizeof(first)) : -1;
  check("procfs_edges: chunked maps read succeeds", total > 0);
  check("procfs_edges: chunked maps reads many VMAs",
        made >= MAP_COUNT / 2 && total > 1024 && count_lines(buf, total) >= made);
  check("procfs_edges: maps EOF reached", eof == 0);
  if (fd >= 0) {
    int n1;
    int n2;
    check("procfs_edges: maps reseek succeeds", lseek(fd, 0, SEEK_SET) == 0);
    n1 = read(fd, first, sizeof(first));
    check("procfs_edges: maps reread after seek", n1 > 0);
    check("procfs_edges: maps second reseek succeeds", lseek(fd, 0, SEEK_SET) == 0);
    n2 = read(fd, again, n1 > 0 ? n1 : 0);
    check("procfs_edges: maps reseek returns same prefix",
          n1 > 0 && n2 == n1 && memcmp(first, again, n1) == 0);
    close(fd);
  }

  for (int i = 0; i < made; i++)
    munmap(maps[i], 4096);
}

static void test_regular_eof_reseek(void) {
  char first[16];
  char again[16];
  char drain[128];
  int fd = open("/proc/self/status", O_RDONLY);
  int n1;
  int eof;

  check("procfs_edges: status open for EOF test", fd >= 0);
  if (fd < 0)
    return;

  n1 = read(fd, first, sizeof(first));
  while (read(fd, drain, sizeof(drain)) > 0)
    ;
  eof = read(fd, drain, sizeof(drain));
  check("procfs_edges: status EOF returns zero", n1 > 0 && eof == 0);
  check("procfs_edges: status seek to start", lseek(fd, 0, SEEK_SET) == 0);
  check("procfs_edges: status reread after seek",
        read(fd, again, n1) == n1 && memcmp(first, again, n1) == 0);
  close(fd);
}

static void test_tiny_getdents(void) {
  char tiny[8];
  char buf[256];
  int fd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY);
  long n;

  check("procfs_edges: fd dir open for getdents", fd >= 0);
  if (fd < 0)
    return;

  n = syscall(SYS_getdents64, fd, tiny, 1);
  check("procfs_edges: tiny getdents returns EINVAL", n == -EINVAL);
  n = syscall(SYS_getdents64, fd, buf, sizeof(buf));
  check("procfs_edges: getdents works after tiny buffer", n > 0);
  close(fd);
}

static int fd_dir_has_entry(int fdnum, ino_t ino) {
  int dirfd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY);
  char buf[512];
  int found = 0;

  if (dirfd < 0)
    return 0;
  for (;;) {
    long n = syscall(SYS_getdents64, dirfd, buf, sizeof(buf));
    int pos = 0;
    if (n < 0) {
      close(dirfd);
      return 0;
    }
    if (n == 0)
      break;
    while (pos < n) {
      struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + pos);
      if (simple_atoi(d->d_name) == fdnum &&
          d->d_type == DT_LNK && d->d_ino == (unsigned long)ino)
        found = 1;
      pos += d->d_reclen;
    }
  }
  close(dirfd);
  return found;
}

static void test_self_and_procfd_links(void) {
  const char *path = "/tmp/procfs-edges-fd";
  char self[32];
  char procpath[64];
  char link[128];
  struct stat st_self_l;
  struct stat st_self;
  struct stat st_link;
  struct stat st_follow;
  struct stat st_fd;
  int fd;
  int n;
  int link_lstat_ok;

  n = readlink("/proc/self", self, sizeof(self) - 1);
  check("procfs_edges: self readlink returns pid", n > 0);
  if (n > 0) {
    self[n] = '\0';
    check("procfs_edges: self readlink matches getpid",
          simple_atoi(self) == (int)getpid());
    check("procfs_edges: self lstat is link",
          lstat("/proc/self", &st_self_l) == 0 &&
          S_ISLNK(st_self_l.st_mode) && st_self_l.st_size == n);
    check("procfs_edges: self stat follows to dir",
          stat("/proc/self", &st_self) == 0 && S_ISDIR(st_self.st_mode));
  }

  n = readlink("/proc/self/root", link, sizeof(link) - 1);
  check("procfs_edges: root readlink returns /", n == 1 && link[0] == '/');
  check("procfs_edges: root lstat is link",
        lstat("/proc/self/root", &st_link) == 0 &&
        S_ISLNK(st_link.st_mode) && st_link.st_size == 1);

  unlink(path);
  fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  check("procfs_edges: procfd source setup", fd >= 0);
  if (fd < 0)
    return;
  check("procfs_edges: procfd source write", write(fd, "xyz", 3) == 3);
  check("procfs_edges: procfd source seek", lseek(fd, 1, SEEK_SET) == 1);
  snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", fd);

  n = readlink(procpath, link, sizeof(link) - 1);
  check("procfs_edges: procfd readlink returns target", n > 0);
  if (n > 0) {
    link[n] = '\0';
    check("procfs_edges: procfd readlink matches path", strcmp(link, path) == 0);
  }
  memset(&st_link, 0, sizeof(st_link));
  link_lstat_ok = lstat(procpath, &st_link) == 0;
  check("procfs_edges: procfd lstat is link",
        link_lstat_ok && S_ISLNK(st_link.st_mode) &&
        st_link.st_size == (off_t)strlen(path));
  check("procfs_edges: procfd readdir inode agrees",
        link_lstat_ok && fd_dir_has_entry(fd, st_link.st_ino));
  check("procfs_edges: procfd stat follows source",
        stat(procpath, &st_follow) == 0 && fstat(fd, &st_fd) == 0 &&
        stat_fstat_match(&st_follow, &st_fd));

  errno = 0;
  int bad = open(procpath, O_RDONLY | O_NOFOLLOW);
  check("procfs_edges: procfd nofollow rejected", bad < 0 && errno == ELOOP);
  if (bad >= 0)
    close(bad);

  int reopened = open(procpath, O_RDONLY);
  check("procfs_edges: procfd O_RDONLY reopen succeeds", reopened >= 0);
  if (reopened >= 0) {
    char c = 0;
    check("procfs_edges: procfd reopen has fresh offset",
          read(reopened, &c, 1) == 1 && c == 'x');
    close(reopened);
  }

  errno = 0;
  bad = open("/proc/self/fd/99bad", O_RDONLY);
  check("procfs_edges: malformed procfd path rejected",
        bad < 0 && errno == ENOENT);
  if (bad >= 0)
    close(bad);

  close(fd);
  unlink(path);
}

int main(void) {
  printf("=== procfs_edges ===\n");
  test_stat_fstat_agreement();
  test_readonly_rejections();
  test_maps_chunked_and_reseek();
  test_regular_eof_reseek();
  test_tiny_getdents();
  test_self_and_procfd_links();
  printf("=== procfs_edges: %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
