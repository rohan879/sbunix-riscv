#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_file(const char *path, const char *data) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  int len = (int)strlen(data);
  if (fd < 0)
    return -1;
  if (write(fd, data, len) != len) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static void probe(const char *label, const char *path) {
  char buf[128];
  int ret;

  memset(buf, 'X', sizeof(buf));
  errno = 0;
  ret = (int)readlink(path, buf, sizeof(buf));
  printf("%s\n", label);
  printf("  path=%s\n", path);
  printf("  ret=%d errno=%d\n", ret, errno);
  if (ret >= 0) {
    int shown = ret < (int)sizeof(buf) ? ret : (int)sizeof(buf);
    printf("  data=");
    for (int i = 0; i < shown; i++)
      putchar(buf[i]);
    putchar('\n');
  }
}

static void probeat(const char *label, int dirfd, const char *path) {
  char buf[128];
  int ret;

  memset(buf, 'X', sizeof(buf));
  errno = 0;
  ret = (int)readlinkat(dirfd, path, buf, sizeof(buf));
  printf("%s\n", label);
  printf("  dirfd=%d path=%s\n", dirfd, path);
  printf("  ret=%d errno=%d\n", ret, errno);
  if (ret >= 0) {
    int shown = ret < (int)sizeof(buf) ? ret : (int)sizeof(buf);
    printf("  data=");
    for (int i = 0; i < shown; i++)
      putchar(buf[i]);
    putchar('\n');
  }
}

int main(void) {
  char cwd[256];
  char procfd_path[64];
  int dirfd;
  char self_pid_path[64];

  printf("=== readlinkdiag ===\n");

  unlink("/mnt/rld_file");
  rmdir("/mnt/rld_dir");
  unlink("/tmp/rld_file");
  rmdir("/tmp/rld_dir");

  printf("setup /mnt file=%d dir=%d\n",
         write_file("/mnt/rld_file", "x"),
         mkdir("/mnt/rld_dir", 0755));
  printf("setup /tmp file=%d dir=%d\n",
         write_file("/tmp/rld_file", "x"),
         mkdir("/tmp/rld_dir", 0755));

  probe("ext2 regular file", "/mnt/rld_file");
  probe("ext2 directory", "/mnt/rld_dir");

  probe("ramfs regular file", "/tmp/rld_file");
  probe("ramfs directory", "/tmp/rld_dir");

  probe("tarfs regular file", "/bin/echo");
  probe("tarfs directory", "/bin");

  probe("tarfs root directory", "/");
  probe("devfs root directory", "/dev");
  probe("devfs regular file", "/dev/null");
  probe("procfs root directory", "/proc");
  probe("procfs self symlink", "/proc/self");
  probe("procfs self fd directory", "/proc/self/fd");
  probe("procfs self fdinfo directory", "/proc/self/fdinfo");
  probe("procfs regular file", "/proc/self/status");
  probe("procfs global meminfo", "/proc/meminfo");
  probe("procfs global mounts", "/proc/mounts");
  probe("procfs global version", "/proc/version");
  probe("procfs tty directory", "/proc/tty");
  probe("procfs tty drivers file", "/proc/tty/drivers");
  probe("procfs tty ldiscs file", "/proc/tty/ldiscs");
  probe("sysfs root directory", "/sys");
  probe("sysfs directory", "/sys/kernel");
  probe("sysfs regular file", "/sys/kernel/version");
  probe("ext2 root directory", "/mnt");
  probe("ramfs root directory", "/tmp");

  if (getcwd(cwd, sizeof(cwd))) {
    if (chdir("/mnt") == 0) {
      probe("relative ext2 regular file", "rld_file");
      probe("relative ext2 directory", "rld_dir");
      chdir(cwd);
    }
    if (chdir("/tmp") == 0) {
      probe("relative ramfs regular file", "rld_file");
      probe("relative ramfs directory", "rld_dir");
      chdir(cwd);
    }
    if (chdir("/bin") == 0) {
      probe("relative tarfs regular file", "echo");
      probe("relative tarfs directory", ".");
      chdir(cwd);
    }
  }

  snprintf(self_pid_path, sizeof(self_pid_path), "/proc/%d", (int)getpid());
  probe("procfs numeric pid directory", self_pid_path);
  snprintf(self_pid_path, sizeof(self_pid_path), "/proc/%d/fd", (int)getpid());
  probe("procfs numeric pid fd directory", self_pid_path);
  snprintf(self_pid_path, sizeof(self_pid_path), "/proc/%d/fdinfo", (int)getpid());
  probe("procfs numeric pid fdinfo directory", self_pid_path);

  probe("procfs root -> ext2 regular file", "/proc/self/root/mnt/rld_file");
  probe("procfs root -> ext2 directory", "/proc/self/root/mnt/rld_dir");
  probe("procfs root -> ramfs regular file", "/proc/self/root/tmp/rld_file");
  probe("procfs root -> ramfs directory", "/proc/self/root/tmp/rld_dir");
  probe("procfs root -> tarfs regular file", "/proc/self/root/bin/echo");
  probe("procfs root -> tarfs directory", "/proc/self/root/bin");

  if (getcwd(cwd, sizeof(cwd))) {
    if (chdir("/tmp") == 0) {
      probe("procfs cwd -> ramfs regular file", "/proc/self/cwd/rld_file");
      probe("procfs cwd -> ramfs directory", "/proc/self/cwd/rld_dir");
      chdir(cwd);
    }
    if (chdir("/mnt") == 0) {
      probe("procfs cwd -> ext2 regular file", "/proc/self/cwd/rld_file");
      probe("procfs cwd -> ext2 directory", "/proc/self/cwd/rld_dir");
      chdir(cwd);
    }
  }

  dirfd = open("/mnt", O_RDONLY | O_DIRECTORY);
  if (dirfd >= 0) {
    snprintf(procfd_path, sizeof(procfd_path), "/proc/self/fd/%d/rld_file", dirfd);
    probe("procfs fd dir -> ext2 regular file", procfd_path);
    snprintf(procfd_path, sizeof(procfd_path), "/proc/self/fd/%d/rld_dir", dirfd);
    probe("procfs fd dir -> ext2 directory", procfd_path);
    snprintf(procfd_path, sizeof(procfd_path), "/proc/self/fdinfo/%d", dirfd);
    probe("procfs fdinfo leaf for ext2 dirfd", procfd_path);
    probeat("readlinkat ext2 dirfd current dir", dirfd, ".");
    probeat("readlinkat ext2 dirfd parent dir", dirfd, "..");
    probeat("readlinkat ext2 dirfd regular file", dirfd, "rld_file");
    probeat("readlinkat ext2 dirfd directory", dirfd, "rld_dir");
    close(dirfd);
  }

  dirfd = open("/tmp", O_RDONLY | O_DIRECTORY);
  if (dirfd >= 0) {
    snprintf(procfd_path, sizeof(procfd_path), "/proc/self/fd/%d/rld_file", dirfd);
    probe("procfs fd dir -> ramfs regular file", procfd_path);
    snprintf(procfd_path, sizeof(procfd_path), "/proc/self/fd/%d/rld_dir", dirfd);
    probe("procfs fd dir -> ramfs directory", procfd_path);
    snprintf(procfd_path, sizeof(procfd_path), "/proc/self/fdinfo/%d", dirfd);
    probe("procfs fdinfo leaf for ramfs dirfd", procfd_path);
    probeat("readlinkat ramfs dirfd current dir", dirfd, ".");
    probeat("readlinkat ramfs dirfd parent dir", dirfd, "..");
    probeat("readlinkat ramfs dirfd regular file", dirfd, "rld_file");
    probeat("readlinkat ramfs dirfd directory", dirfd, "rld_dir");
    close(dirfd);
  }

  unlink("/mnt/rld_file");
  rmdir("/mnt/rld_dir");
  unlink("/tmp/rld_file");
  rmdir("/tmp/rld_dir");

  return 0;
}
