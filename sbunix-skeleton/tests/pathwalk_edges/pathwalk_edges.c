#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int read_file(const char *path, char *buf, int bufsz) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  int n = read(fd, buf, bufsz - 1);
  close(fd);
  if (n < 0)
    return -1;
  buf[n] = '\0';
  return n;
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

static void chain_name(char *buf, int bufsz, const char *prefix, int idx) {
  snprintf(buf, bufsz, "/mnt/%s%d", prefix, idx);
}

static void cleanup_chain(const char *prefix, int max_idx) {
  char path[64];
  for (int i = 0; i <= max_idx; i++) {
    chain_name(path, sizeof(path), prefix, i);
    unlink(path);
  }
}

static int check_chain_limit(void) {
  int pass = 1;
  char cur[64];
  char next[64];
  char buf[32];

  cleanup_chain("pws", 41);
  cleanup_chain("pwe", 42);

  chain_name(cur, sizeof(cur), "pws", 40);
  if (write_file(cur, "ok") < 0) {
    printf("[FAIL] pathwalk_edges: chain setup errno=%d\n", errno);
    return 0;
  }
  for (int i = 39; i >= 0; i--) {
    chain_name(cur, sizeof(cur), "pws", i);
    chain_name(next, sizeof(next), "pws", i + 1);
    if (symlink(next, cur) < 0) {
      printf("[FAIL] pathwalk_edges: chain symlink errno=%d\n", errno);
      return 0;
    }
  }
  chain_name(cur, sizeof(cur), "pws", 0);
  if (read_file(cur, buf, sizeof(buf)) != 2 || strcmp(buf, "ok") != 0) {
    printf("[FAIL] pathwalk_edges: 40-hop chain did not resolve errno=%d\n",
           errno);
    pass = 0;
  }

  chain_name(cur, sizeof(cur), "pwe", 41);
  write_file(cur, "no");
  for (int i = 40; i >= 0; i--) {
    chain_name(cur, sizeof(cur), "pwe", i);
    chain_name(next, sizeof(next), "pwe", i + 1);
    symlink(next, cur);
  }
  chain_name(cur, sizeof(cur), "pwe", 0);
  errno = 0;
  int fd = open(cur, O_RDONLY);
  if (fd >= 0 || errno != ELOOP) {
    printf("[FAIL] pathwalk_edges: 41-hop chain errno=%d\n", errno);
    pass = 0;
    if (fd >= 0)
      close(fd);
  }

  cleanup_chain("pws", 41);
  cleanup_chain("pwe", 42);
  return pass;
}

static int check_loops(void) {
  int pass = 1;
  int fd;

  unlink("/mnt/pw_loop_self");
  unlink("/mnt/pw_loop_a");
  unlink("/mnt/pw_loop_b");
  symlink("/mnt/pw_loop_self", "/mnt/pw_loop_self");
  errno = 0;
  fd = open("/mnt/pw_loop_self", O_RDONLY);
  if (fd >= 0 || errno != ELOOP) {
    printf("[FAIL] pathwalk_edges: self loop errno=%d\n", errno);
    pass = 0;
    if (fd >= 0)
      close(fd);
  }

  symlink("/mnt/pw_loop_b", "/mnt/pw_loop_a");
  symlink("/mnt/pw_loop_a", "/mnt/pw_loop_b");
  errno = 0;
  fd = open("/mnt/pw_loop_a", O_RDONLY);
  if (fd >= 0 || errno != ELOOP) {
    printf("[FAIL] pathwalk_edges: two-node loop errno=%d\n", errno);
    pass = 0;
    if (fd >= 0)
      close(fd);
  }

  unlink("/mnt/pw_loop_self");
  unlink("/mnt/pw_loop_a");
  unlink("/mnt/pw_loop_b");
  return pass;
}

static int check_link_symlink(void) {
  int pass = 1;
  struct stat st;
  char buf[128];
  int n;

  unlink("/mnt/pw_hard_target");
  unlink("/mnt/pw_hard_symlink");
  unlink("/mnt/pw_hard_copy");
  unlink("/mnt/pw_hard_follow");
  write_file("/mnt/pw_hard_target", "target");
  if (symlink("/mnt/pw_hard_target", "/mnt/pw_hard_symlink") < 0) {
    printf("[FAIL] pathwalk_edges: symlink setup errno=%d\n", errno);
    return 0;
  }
  if (link("/mnt/pw_hard_symlink", "/mnt/pw_hard_copy") < 0) {
    printf("[FAIL] pathwalk_edges: hard link symlink errno=%d\n", errno);
    return 0;
  }
  if (lstat("/mnt/pw_hard_copy", &st) < 0 || !S_ISLNK(st.st_mode)) {
    printf("[FAIL] pathwalk_edges: hard link is not symlink errno=%d\n", errno);
    pass = 0;
  }
  n = readlink("/mnt/pw_hard_copy", buf, sizeof(buf) - 1);
  if (n < 0) {
    printf("[FAIL] pathwalk_edges: hard link readlink errno=%d\n", errno);
    pass = 0;
  } else {
    buf[n] = '\0';
    if (strcmp(buf, "/mnt/pw_hard_target") != 0) {
      printf("[FAIL] pathwalk_edges: hard link target=%s\n", buf);
      pass = 0;
    }
  }
  if (linkat(AT_FDCWD, "/mnt/pw_hard_symlink", AT_FDCWD,
             "/mnt/pw_hard_follow", AT_SYMLINK_FOLLOW) < 0) {
    printf("[FAIL] pathwalk_edges: follow hard link errno=%d\n", errno);
    pass = 0;
  } else if (lstat("/mnt/pw_hard_follow", &st) < 0 ||
             !S_ISREG(st.st_mode)) {
    printf("[FAIL] pathwalk_edges: follow hard link is not regular errno=%d\n",
           errno);
    pass = 0;
  } else if (read_file("/mnt/pw_hard_follow", buf, sizeof(buf)) != 6 ||
             strcmp(buf, "target") != 0) {
    printf("[FAIL] pathwalk_edges: follow hard link content\n");
    pass = 0;
  }
  unlink("/mnt/pw_hard_follow");
  unlink("/mnt/pw_hard_copy");
  unlink("/mnt/pw_hard_symlink");
  unlink("/mnt/pw_hard_target");
  return pass;
}

static int check_long_paths(void) {
  int pass = 1;
  char ok_path[256];
  char bad_path[257];
  int prefix_len = 5;

  memcpy(ok_path, "/mnt/", prefix_len);
  for (int i = prefix_len; i < 255; i++)
    ok_path[i] = 'a';
  ok_path[255] = '\0';

  memcpy(bad_path, "/mnt/", prefix_len);
  for (int i = prefix_len; i < 256; i++)
    bad_path[i] = 'b';
  bad_path[256] = '\0';

  unlink(ok_path);
  if (write_file(ok_path, "x") < 0) {
    printf("[FAIL] pathwalk_edges: max path create errno=%d\n", errno);
    pass = 0;
  } else {
    unlink(ok_path);
  }

  errno = 0;
  int fd = open(bad_path, O_RDONLY);
  if (fd >= 0 || errno != ENAMETOOLONG) {
    printf("[FAIL] pathwalk_edges: overlong path errno=%d\n", errno);
    pass = 0;
    if (fd >= 0)
      close(fd);
  }

  return pass;
}

int main(void) {
  int pass = 1;

  if (!check_chain_limit())
    pass = 0;
  if (!check_loops())
    pass = 0;
  if (!check_link_symlink())
    pass = 0;
  if (!check_long_paths())
    pass = 0;

  if (pass)
    printf("[PASS] pathwalk_edges: symlink and path edges\n");
  return pass ? 0 : 1;
}
