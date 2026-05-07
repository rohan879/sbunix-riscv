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
    int want = bufsz - 1 - total;
    int n;
    if (want > 7)
      want = 7;
    n = read(fd, buf + total, want);
    if (n < 0) {
      close(fd);
      return -1;
    }
    if (n == 0)
      break;
    total += n;
  }
  {
    char c;
    int eof = read(fd, &c, 1);
    close(fd);
    if (eof != 0)
      return -1;
  }
  buf[total] = '\0';
  return total;
}

static int dir_has(const char *path, const char *name) {
  DIR *dir = opendir(path);
  struct dirent *de;

  if (!dir)
    return 0;
  while ((de = readdir(dir)) != 0) {
    if (strcmp(de->d_name, name) == 0) {
      closedir(dir);
      return 1;
    }
  }
  closedir(dir);
  return 0;
}

static void expect_file_contains(const char *path, const char *needle) {
  char buf[512];
  struct stat st;
  int n = read_file(path, buf, sizeof(buf));

  check(path, n > 0 && strstr(buf, needle) != 0);
  check("procfs_global_edges: stat regular",
        stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_size >= 0);
}

int main(void) {
  struct stat st;

  printf("=== procfs_global_edges ===\n");

  check("procfs_global_edges: root lists filesystems",
        dir_has("/proc", "filesystems"));
  check("procfs_global_edges: root lists tty", dir_has("/proc", "tty"));
  check("procfs_global_edges: tty dir stat",
        stat("/proc/tty", &st) == 0 && S_ISDIR(st.st_mode));
  check("procfs_global_edges: tty lists drivers",
        dir_has("/proc/tty", "drivers"));
  check("procfs_global_edges: tty lists ldiscs",
        dir_has("/proc/tty", "ldiscs"));

  expect_file_contains("/proc/filesystems", "proc");
  expect_file_contains("/proc/cmdline", "console=");
  expect_file_contains("/proc/devices", "Character devices:");
  expect_file_contains("/proc/partitions", "major minor");
  expect_file_contains("/proc/consoles", "ttyS0");
  expect_file_contains("/proc/tty/drivers", "serial");
  expect_file_contains("/proc/tty/ldiscs", "n_tty");

  printf("=== procfs_global_edges: %d passed, %d failed ===\n",
         g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
