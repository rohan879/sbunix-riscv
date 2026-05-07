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

static int readlink_text(const char *path, char *buf, int bufsz) {
  int n = readlink(path, buf, bufsz - 1);
  if (n < 0)
    return -1;
  buf[n] = '\0';
  return n;
}

static int open_read_text(const char *path, char *buf, int bufsz) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;

  int total = 0;
  while (total < bufsz - 1) {
    int want = bufsz - 1 - total;
    if (want > 3)
      want = 3;
    int n = read(fd, buf + total, want);
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

static void compare_cwd_views(const char *label) {
  char cwd[256];
  char numeric[64];
  char self_link[256];
  char self_read[256];
  char pid_link[256];
  char pid_read[256];

  if (!getcwd(cwd, sizeof(cwd))) {
    check(label, 0);
    return;
  }

  snprintf(numeric, sizeof(numeric), "/proc/%d/cwd", (int)getpid());
  int n_pid_link = readlink_text(numeric, pid_link, sizeof(pid_link));
  int n_pid_read = open_read_text(numeric, pid_read, sizeof(pid_read));
  int n_self_link = readlink_text("/proc/self/cwd", self_link,
                                  sizeof(self_link));
  int n_self_read = open_read_text("/proc/self/cwd", self_read,
                                   sizeof(self_read));

  printf("procfs_cwd_edges: %s getcwd=%s\n", label, cwd);
  if (n_pid_link >= 0)
    printf("procfs_cwd_edges: %s numeric readlink=%s\n", label, pid_link);
  if (n_pid_read >= 0)
    printf("procfs_cwd_edges: %s numeric open-read=%s\n", label, pid_read);
  if (n_self_link >= 0)
    printf("procfs_cwd_edges: %s self readlink=%s\n", label, self_link);
  if (n_self_read >= 0)
    printf("procfs_cwd_edges: %s self open-read=%s\n", label, self_read);

  check("procfs_cwd_edges: getcwd absolute", cwd[0] == '/');
  check("procfs_cwd_edges: numeric readlink matches getcwd",
        n_pid_link >= 0 && pid_link[0] == '/' && strcmp(pid_link, cwd) == 0);
  check("procfs_cwd_edges: numeric open/read matches getcwd",
        n_pid_read >= 0 && pid_read[0] == '/' && strcmp(pid_read, cwd) == 0);
  check("procfs_cwd_edges: self readlink matches numeric",
        n_self_link >= 0 && strcmp(self_link, pid_link) == 0);
  check("procfs_cwd_edges: self open/read matches numeric",
        n_self_read >= 0 && strcmp(self_read, pid_read) == 0);

  struct stat st_lnk;
  struct stat st_dir;
  check("procfs_cwd_edges: lstat reports cwd symlink",
        lstat(numeric, &st_lnk) == 0 && S_ISLNK(st_lnk.st_mode));
  check("procfs_cwd_edges: stat follows cwd symlink",
        stat(numeric, &st_dir) == 0 && S_ISDIR(st_dir.st_mode));
}

int main(void) {
  printf("=== procfs_cwd_edges ===\n");

  mkdir("/tmp/procfs_cwd_edges_dir", 0755);
  mkdir("/tmp/procfs_cwd_edges_dir/nested", 0755);

  check("procfs_cwd_edges: chdir root", chdir("/") == 0);
  compare_cwd_views("root");

  check("procfs_cwd_edges: chdir nested",
        chdir("/tmp/procfs_cwd_edges_dir/nested") == 0);
  compare_cwd_views("nested");

  chdir("/");
  rmdir("/tmp/procfs_cwd_edges_dir/nested");
  rmdir("/tmp/procfs_cwd_edges_dir");

  printf("=== procfs_cwd_edges: %d passed, %d failed ===\n",
         g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
