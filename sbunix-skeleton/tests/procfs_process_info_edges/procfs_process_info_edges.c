#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_pass;
static int g_fail;

#define LONG_ENV_LEN 1500

static void check(const char *name, int ok) {
  if (ok) {
    g_pass++;
    printf("[PASS] %s\n", name);
  } else {
    g_fail++;
    printf("[FAIL] %s errno=%d\n", name, errno);
  }
}

static int read_all(const char *path, char *buf, int bufsz, int chunk) {
  int fd = open(path, O_RDONLY);
  int total = 0;

  if (fd < 0)
    return -1;
  memset(buf, 'Z', bufsz);
  while (total < bufsz) {
    int want = bufsz - total;
    int n;
    if (want > chunk)
      want = chunk;
    n = read(fd, buf + total, want);
    if (n < 0) {
      close(fd);
      return -1;
    }
    if (n == 0)
      break;
    total += n;
  }
  close(fd);
  return total;
}

static int stat_field_count(const char *buf, int len) {
  const char *end = buf + len;
  const char *lp = 0;
  const char *rp = 0;
  const char *p;
  int fields = 0;

  for (p = buf; p < end; p++) {
    if (*p == '(' && !lp)
      lp = p;
    if (*p == ')')
      rp = p;
  }
  if (!lp || !rp || rp <= lp)
    return 0;
  fields = 2;
  p = rp + 1;
  while (p < end && *p == ' ')
    p++;
  if (p < end && *p != '\n') {
    fields++;
    while (p < end && *p != ' ' && *p != '\n')
      p++;
  }
  while (p < end) {
    while (p < end && *p == ' ')
      p++;
    if (p >= end || *p == '\n')
      break;
    fields++;
    while (p < end && *p != ' ' && *p != '\n')
      p++;
  }
  return fields;
}

static int statm_has_seven_numbers(const char *buf, int len) {
  int fields = 0;
  int i = 0;

  while (i < len && buf[i] != '\n') {
    if (buf[i] < '0' || buf[i] > '9')
      return 0;
    while (i < len && buf[i] >= '0' && buf[i] <= '9')
      i++;
    fields++;
    if (fields == 7)
      break;
    if (i >= len || buf[i] != ' ')
      return 0;
    i++;
  }
  return fields == 7 && i < len && buf[i] == '\n' && i + 1 == len;
}

static int environ_child(void) {
  static const char env0[] = "PFS_ENV=hello";
  static const char env1[] = "SECOND=two";
  char buf[64];
  int n = read_all("/proc/self/environ", buf, sizeof(buf), 3);
  int want = (int)sizeof(env0) + (int)sizeof(env1);
  int ok = n == want &&
           memcmp(buf, env0, sizeof(env0)) == 0 &&
           memcmp(buf + sizeof(env0), env1, sizeof(env1)) == 0;
  printf("[%s] procfs_process_info_edges: child environ exact bytes\n",
         ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

static void fill_long_env(char *buf) {
  static const char prefix[] = "LONG_ENV=";
  int pos = (int)sizeof(prefix) - 1;

  memcpy(buf, prefix, pos);
  while (pos < LONG_ENV_LEN) {
    buf[pos] = (char)('a' + (pos % 26));
    pos++;
  }
  buf[LONG_ENV_LEN] = '\0';
}

static int environ_long_child(void) {
  static char expected[LONG_ENV_LEN + 1];
  static char buf[LONG_ENV_LEN + 64];
  struct stat st;
  int n;

  fill_long_env(expected);
  n = read_all("/proc/self/environ", buf, sizeof(buf), 113);
  check("procfs_process_info_edges: long environ stat size",
        stat("/proc/self/environ", &st) == 0 &&
        st.st_size == (long)sizeof(expected));
  check("procfs_process_info_edges: long environ full read",
        n == (int)sizeof(expected) &&
        memcmp(buf, expected, sizeof(expected)) == 0);
  check("procfs_process_info_edges: long environ EOF no implicit NUL",
        n >= 0 && n < (int)sizeof(buf) && buf[n] == 'Z');
  return g_fail == 0 ? 0 : 1;
}

static void test_stat_statm_comm_root(void) {
  char buf[512];
  struct stat st;
  int n;

  n = read_all("/proc/self/stat", buf, sizeof(buf), 17);
  check("procfs_process_info_edges: stat reads", n > 0);
  check("procfs_process_info_edges: stat has 52 fields",
        n > 0 && stat_field_count(buf, n) == 52);
  check("procfs_process_info_edges: stat EOF has no implicit NUL",
        n >= 0 && n < (int)sizeof(buf) && buf[n] == 'Z');

  n = read_all("/proc/self/statm", buf, sizeof(buf), 2);
  check("procfs_process_info_edges: statm reads", n > 0);
  check("procfs_process_info_edges: statm has seven fields",
        n > 0 && statm_has_seven_numbers(buf, n));
  check("procfs_process_info_edges: statm stat is regular",
        stat("/proc/self/statm", &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_size == n);

  n = read_all("/proc/self/comm", buf, sizeof(buf), 1);
  check("procfs_process_info_edges: comm reads", n > 1);
  check("procfs_process_info_edges: comm is truncated linux name",
        n > 1 && n <= 16 && buf[n - 1] == '\n' &&
        memchr(buf, '/', n) == 0);
  check("procfs_process_info_edges: comm no implicit NUL",
        n >= 0 && n < (int)sizeof(buf) && buf[n] == 'Z');

  n = readlink("/proc/self/root", buf, sizeof(buf));
  check("procfs_process_info_edges: root readlink slash",
        n == 1 && buf[0] == '/');
  check("procfs_process_info_edges: root lstat is link",
        lstat("/proc/self/root", &st) == 0 && S_ISLNK(st.st_mode) &&
        st.st_size == 1);
  check("procfs_process_info_edges: root stat follows directory",
        stat("/proc/self/root", &st) == 0 && S_ISDIR(st.st_mode));
}

static void test_environ_exec_snapshot(const char *self_path) {
  char *argv[] = { (char *)self_path, (char *)"--env-child", 0 };
  char *envp[] = { (char *)"PFS_ENV=hello", (char *)"SECOND=two", 0 };
  int status = 0;
  int pid = fork();

  check("procfs_process_info_edges: fork for environ child", pid >= 0);
  if (pid == 0) {
    execve(self_path, argv, envp);
    _exit(127);
  }
  if (pid > 0) {
    int waited = waitpid(pid, &status, 0);
    check("procfs_process_info_edges: environ child exits cleanly",
          waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
}

static void test_long_environ_exec_snapshot(const char *self_path) {
  static char long_env[LONG_ENV_LEN + 1];
  char *argv[] = { (char *)self_path, (char *)"--env-long-child", 0 };
  char *envp[] = { long_env, 0 };
  int status = 0;
  int pid;

  fill_long_env(long_env);
  pid = fork();
  check("procfs_process_info_edges: fork for long environ child", pid >= 0);
  if (pid == 0) {
    execve(self_path, argv, envp);
    _exit(127);
  }
  if (pid > 0) {
    int waited = waitpid(pid, &status, 0);
    check("procfs_process_info_edges: long environ child exits cleanly",
          waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
}

int main(int argc, char **argv) {
  printf("=== procfs_process_info_edges ===\n");
  if (argc > 1 && strcmp(argv[1], "--env-child") == 0)
    return environ_child();
  if (argc > 1 && strcmp(argv[1], "--env-long-child") == 0)
    return environ_long_child();

  test_stat_statm_comm_root();
  test_environ_exec_snapshot(argv[0]);
  test_long_environ_exec_snapshot(argv[0]);

  printf("=== procfs_process_info_edges: %d passed, %d failed ===\n",
         g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
