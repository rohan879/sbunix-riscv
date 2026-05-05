#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int same_file_stat(const struct stat *a, const struct stat *b) {
  return a->st_dev == b->st_dev &&
         a->st_ino == b->st_ino &&
         ((a->st_mode & S_IFMT) == (b->st_mode & S_IFMT));
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

static void make_fd_path(char *buf, int bufsz, const char *pid, int fd) {
  snprintf(buf, bufsz, "/proc/%s/fd/%d", pid, fd);
}

static int check_exec_link(void) {
  char expected[128];
  int expected_len = readlink("/bin/sleep", expected, sizeof(expected) - 1);
  if (expected_len > 0) {
    expected[expected_len] = '\0';
  } else {
    snprintf(expected, sizeof(expected), "%s", "/bin/sleep");
  }

  pid_t pid = fork();
  if (pid < 0) {
    printf("[FAIL] procfs_execfd: fork errno=%d\n", errno);
    return 0;
  }
  if (pid == 0) {
    char *argv[] = { "sleep", "10", 0 };
    execv("/bin/sleep", argv);
    _exit(111);
  }

  char path[64];
  char link[128];
  char last_link[128];
  int last_errno = 0;
  int ok = 0;
  last_link[0] = '\0';
  snprintf(path, sizeof(path), "/proc/%d/exe", (int)pid);
  /* BF: Let the child reach exec before sampling its procfs link.  The
   * inherited pre-exec link is not the state this test is validating. */
  sleep(1);
  for (int i = 0; i < 8; i++) {
    int n = readlink(path, link, sizeof(link) - 1);
    if (n > 0) {
      link[n] = '\0';
      snprintf(last_link, sizeof(last_link), "%s", link);
      if (strcmp(link, expected) == 0) {
        ok = 1;
        break;
      }
    } else
      last_errno = errno;
    sleep(1);
  }

  kill(pid, SIGKILL);
  waitpid(pid, 0, 0);
  if (!ok)
    printf("[FAIL] procfs_execfd: exe link target=%s expected=%s errno=%d\n",
           last_link[0] ? last_link : "<none>", expected, last_errno);
  return ok;
}

static int check_self_fd_link(void) {
  const char *src = "/mnt/procfs_fd_source";
  int pass = 1;
  int fd;
  int dupfd;
  char fdpath[64];
  char link[128];
  struct stat st_link;
  struct stat st_follow;
  struct stat st_fd;
  char c;

  unlink(src);
  if (write_file(src, "abcdef") < 0) {
    printf("[FAIL] procfs_execfd: source setup errno=%d\n", errno);
    return 0;
  }
  fd = open(src, O_RDWR);
  if (fd < 0) {
    printf("[FAIL] procfs_execfd: source open errno=%d\n", errno);
    return 0;
  }
  make_fd_path(fdpath, sizeof(fdpath), "self", fd);

  if (lstat(fdpath, &st_link) < 0 || !S_ISLNK(st_link.st_mode)) {
    printf("[FAIL] procfs_execfd: fd lstat is not symlink errno=%d\n", errno);
    pass = 0;
  }

  int n = readlink(fdpath, link, sizeof(link) - 1);
  if (n < 0) {
    printf("[FAIL] procfs_execfd: fd readlink errno=%d\n", errno);
    pass = 0;
  } else {
    link[n] = '\0';
    if (strcmp(link, src) != 0) {
      printf("[FAIL] procfs_execfd: fd readlink target=%s\n", link);
      pass = 0;
    }
  }

  if (stat(fdpath, &st_follow) < 0 || fstat(fd, &st_fd) < 0 ||
      !same_file_stat(&st_follow, &st_fd)) {
    printf("[FAIL] procfs_execfd: stat does not match fstat errno=%d\n", errno);
    pass = 0;
  }

  errno = 0;
  dupfd = open(fdpath, O_RDONLY | O_DIRECTORY);
  if (dupfd >= 0 || errno != ENOTDIR) {
    printf("[FAIL] procfs_execfd: directory open errno=%d\n", errno);
    pass = 0;
    if (dupfd >= 0)
      close(dupfd);
  }

  if (lseek(fd, 3, SEEK_SET) != 3) {
    printf("[FAIL] procfs_execfd: seek setup errno=%d\n", errno);
    pass = 0;
  }
  dupfd = open(fdpath, O_RDONLY);
  if (dupfd < 0) {
    printf("[FAIL] procfs_execfd: fd open errno=%d\n", errno);
    pass = 0;
  } else {
    c = 0;
    if (read(dupfd, &c, 1) != 1 || c != 'a') {
      printf("[FAIL] procfs_execfd: fd open did not use fresh offset\n");
      pass = 0;
    }
    c = 0;
    if (read(fd, &c, 1) != 1 || c != 'd') {
      printf("[FAIL] procfs_execfd: original offset changed\n");
      pass = 0;
    }
    close(dupfd);
  }

  errno = 0;
  dupfd = open(fdpath, O_WRONLY | O_TRUNC);
  if (dupfd < 0) {
    printf("[FAIL] procfs_execfd: truncate reopen errno=%d\n", errno);
    pass = 0;
  } else {
    struct stat st_trunc;
    if (fstat(fd, &st_trunc) < 0 || st_trunc.st_size != 0) {
      printf("[FAIL] procfs_execfd: truncate did not affect source\n");
      pass = 0;
    }
    close(dupfd);
  }

  errno = 0;
  dupfd = open("/proc/self/fd/63", O_RDONLY);
  if (dupfd >= 0 || errno != ENOENT) {
    printf("[FAIL] procfs_execfd: invalid fd errno=%d\n", errno);
    pass = 0;
    if (dupfd >= 0)
      close(dupfd);
  }

  close(fd);
  unlink(src);
  return pass;
}

static int check_other_process_fd(void) {
  int pfd[2];
  char msg[16];
  int n;
  int child_fd;
  char path[64];
  int fd;
  int pass = 1;

  if (pipe(pfd) < 0) {
    printf("[FAIL] procfs_execfd: pipe errno=%d\n", errno);
    return 0;
  }
  write_file("/mnt/procfs_other_source", "other");

  pid_t pid = fork();
  if (pid < 0) {
    printf("[FAIL] procfs_execfd: fork other errno=%d\n", errno);
    close(pfd[0]);
    close(pfd[1]);
    return 0;
  }
  if (pid == 0) {
    close(pfd[0]);
    int ofd = open("/mnt/procfs_other_source", O_RDONLY);
    snprintf(msg, sizeof(msg), "%d", ofd);
    write(pfd[1], msg, strlen(msg) + 1);
    sleep(5);
    _exit(0);
  }

  close(pfd[1]);
  memset(msg, 0, sizeof(msg));
  n = read(pfd[0], msg, sizeof(msg) - 1);
  close(pfd[0]);
  if (n <= 0) {
    printf("[FAIL] procfs_execfd: child fd read failed\n");
    pass = 0;
  } else {
    child_fd = 0;
    for (int i = 0; msg[i] >= '0' && msg[i] <= '9'; i++)
      child_fd = child_fd * 10 + (msg[i] - '0');
    snprintf(path, sizeof(path), "/proc/%d/fd/%d", (int)pid, child_fd);
    errno = 0;
    fd = open(path, O_RDONLY);
    if (fd >= 0 || errno != EACCES) {
      printf("[FAIL] procfs_execfd: other fd open errno=%d\n", errno);
      pass = 0;
      if (fd >= 0)
        close(fd);
    }
  }

  kill(pid, SIGKILL);
  waitpid(pid, 0, 0);
  unlink("/mnt/procfs_other_source");
  return pass;
}

static int check_pipe_fd(void) {
  int pfd[2];
  char path[64];
  int fd;
  int pass = 1;

  if (pipe(pfd) < 0) {
    printf("[FAIL] procfs_execfd: pipe setup errno=%d\n", errno);
    return 0;
  }
  make_fd_path(path, sizeof(path), "self", pfd[0]);
  errno = 0;
  fd = open(path, O_WRONLY);
  if (fd < 0) {
    printf("[FAIL] procfs_execfd: pipe fd reopen errno=%d\n", errno);
    pass = 0;
  } else {
    char c = 0;
    if (write(fd, "q", 1) != 1 || read(pfd[0], &c, 1) != 1 || c != 'q') {
      printf("[FAIL] procfs_execfd: pipe fd reopen data failed\n");
      pass = 0;
    }
    close(fd);
  }
  close(pfd[0]);
  close(pfd[1]);
  return pass;
}

int main(void) {
  int pass = 1;

  if (!check_exec_link())
    pass = 0;
  if (!check_self_fd_link())
    pass = 0;
  if (!check_other_process_fd())
    pass = 0;
  if (!check_pipe_fd())
    pass = 0;

  if (pass)
    printf("[PASS] procfs_execfd: executable and fd links\n");
  return pass ? 0 : 1;
}
