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

static int read_chunks(const char *path, char *buf, int bufsz) {
  int fd = open(path, O_RDONLY);
  int total = 0;

  if (fd < 0)
    return -1;

  while (total < bufsz - 1) {
    int want = bufsz - 1 - total;
    int n;
    if (want > 4)
      want = 4;
    n = read(fd, buf + total, want);
    if (n < 0) {
      close(fd);
      return -1;
    }
    if (n == 0)
      break;
    total += n;
  }

  char extra;
  int eof = read(fd, &extra, 1);
  close(fd);
  if (eof != 0)
    return -1;
  buf[total] = '\0';
  return total;
}

static int parse_uint(const char **p, int *out) {
  int v = 0;
  int digits = 0;

  while (**p >= '0' && **p <= '9') {
    v = v * 10 + (**p - '0');
    (*p)++;
    digits++;
  }
  if (digits == 0)
    return -1;
  *out = v;
  return 0;
}

static int parse_loadavg(const char *buf, int *runnable, int *total,
                         int *last_pid) {
  const char *p = buf;
  const char prefix[] = "0.00 0.00 0.00 ";

  if (strncmp(p, prefix, sizeof(prefix) - 1) != 0)
    return -1;
  p += sizeof(prefix) - 1;

  if (parse_uint(&p, runnable) < 0 || *p != '/')
    return -1;
  p++;
  if (parse_uint(&p, total) < 0 || *p != ' ')
    return -1;
  p++;
  if (parse_uint(&p, last_pid) < 0 || *p != '\n')
    return -1;
  p++;
  return *p == '\0' ? 0 : -1;
}

static int proc_root_has_loadavg(void) {
  DIR *dir = opendir("/proc");
  struct dirent *de;

  if (!dir)
    return 0;
  while ((de = readdir(dir)) != 0) {
    if (strcmp(de->d_name, "loadavg") == 0) {
      closedir(dir);
      return 1;
    }
  }
  closedir(dir);
  return 0;
}

int main(void) {
  char buf[128];
  int n;
  int runnable = -1;
  int total = -1;
  int last_pid = -1;
  int pid = (int)getpid();
  struct stat st_path;
  struct stat st_fd;
  int fd;
  int stat_ok;
  char linkbuf[16];

  printf("=== procfs_loadavg_edges ===\n");

  n = read_chunks("/proc/loadavg", buf, sizeof(buf));
  printf("procfs_loadavg_edges: read=%s", n >= 0 ? buf : "<failed>\n");
  check("procfs_loadavg_edges: read succeeds", n > 0);
  check("procfs_loadavg_edges: parses linux-shaped line",
        n > 0 && parse_loadavg(buf, &runnable, &total, &last_pid) == 0);
  check("procfs_loadavg_edges: live counts are sane",
        runnable >= 1 && total >= runnable && last_pid >= pid);

  check("procfs_loadavg_edges: proc root lists loadavg",
        proc_root_has_loadavg());

  fd = open("/proc/loadavg", O_RDONLY);
  stat_ok = stat("/proc/loadavg", &st_path) == 0 && S_ISREG(st_path.st_mode);
  check("procfs_loadavg_edges: open succeeds", fd >= 0);
  check("procfs_loadavg_edges: stat reports regular file", stat_ok);
  check("procfs_loadavg_edges: fstat size matches read",
        fd >= 0 && stat_ok && fstat(fd, &st_fd) == 0 &&
        S_ISREG(st_fd.st_mode) &&
        st_fd.st_size == st_path.st_size && st_fd.st_size == n);
  if (fd >= 0)
    close(fd);

  errno = 0;
  fd = open("/proc/loadavg", O_WRONLY);
  check("procfs_loadavg_edges: write-only open rejected",
        fd < 0 && errno == EACCES);
  if (fd >= 0)
    close(fd);

  errno = 0;
  fd = open("/proc/loadavg", O_RDONLY | O_TRUNC);
  check("procfs_loadavg_edges: truncating open rejected",
        fd < 0 && errno == EROFS);
  if (fd >= 0)
    close(fd);

  errno = 0;
  check("procfs_loadavg_edges: readlink rejects non-link",
        readlink("/proc/loadavg", linkbuf, sizeof(linkbuf)) < 0 &&
        errno == EINVAL);

  printf("=== procfs_loadavg_edges: %d passed, %d failed ===\n",
         g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
