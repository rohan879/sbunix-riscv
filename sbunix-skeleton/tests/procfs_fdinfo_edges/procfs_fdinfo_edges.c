#include <dirent.h>
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

static int read_file(const char *path, char *buf, int bufsz) {
  int fd = open(path, O_RDONLY);
  int total = 0;

  if (fd < 0)
    return -1;
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

static int fdinfo_dir_has(int fd) {
  DIR *dir = opendir("/proc/self/fdinfo");
  struct dirent *de;
  char want[16];

  if (!dir)
    return 0;
  snprintf(want, sizeof(want), "%d", fd);
  while ((de = readdir(dir)) != 0) {
    if (strcmp(de->d_name, want) == 0) {
      closedir(dir);
      return 1;
    }
  }
  closedir(dir);
  return 0;
}

int main(void) {
  const char *path = "/tmp/procfs_fdinfo_edges_file";
  char procpath[64];
  char buf[256];
  struct stat st;
  int fd;
  int n;

  printf("=== procfs_fdinfo_edges ===\n");

  unlink(path);
  fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  check("procfs_fdinfo_edges: source open", fd >= 0);
  if (fd >= 0) {
    check("procfs_fdinfo_edges: source write", write(fd, "abcdef", 6) == 6);
    check("procfs_fdinfo_edges: source seek", lseek(fd, 3, SEEK_SET) == 3);
    snprintf(procpath, sizeof(procpath), "/proc/self/fdinfo/%d", fd);

    n = read_file(procpath, buf, sizeof(buf));
    check("procfs_fdinfo_edges: fdinfo reads", n > 0);
    check("procfs_fdinfo_edges: fdinfo has pos",
          n > 0 && strstr(buf, "pos:\t3\n") != 0);
    check("procfs_fdinfo_edges: fdinfo has flags",
          n > 0 && strstr(buf, "flags:\t") != 0);
    check("procfs_fdinfo_edges: fdinfo has mnt_id",
          n > 0 && strstr(buf, "mnt_id:\t0\n") != 0);
    check("procfs_fdinfo_edges: fdinfo has ino",
          n > 0 && strstr(buf, "ino:\t") != 0);
    check("procfs_fdinfo_edges: stat reports regular",
          stat(procpath, &st) == 0 && S_ISREG(st.st_mode) &&
          st.st_size == n);
    check("procfs_fdinfo_edges: directory lists fd", fdinfo_dir_has(fd));

    close(fd);
  }

  errno = 0;
  n = read_file("/proc/self/fdinfo/99", buf, sizeof(buf));
  check("procfs_fdinfo_edges: missing fdinfo is ENOENT",
        n < 0 && errno == ENOENT);

  check("procfs_fdinfo_edges: fdinfo dir stat",
        stat("/proc/self/fdinfo", &st) == 0 && S_ISDIR(st.st_mode));

  unlink(path);
  printf("=== procfs_fdinfo_edges: %d passed, %d failed ===\n",
         g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
