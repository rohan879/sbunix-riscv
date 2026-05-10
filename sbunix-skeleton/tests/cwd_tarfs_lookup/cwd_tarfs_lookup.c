#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_pass;
static int g_fail;

static void check(const char *label, int ok) {
  if (ok) {
    g_pass++;
    printf("[PASS] cwd_tarfs_lookup: %s\n", label);
  } else {
    g_fail++;
    printf("[FAIL] cwd_tarfs_lookup: %s errno=%d\n", label, errno);
  }
}

static int same_file_stat(const struct stat *a, const struct stat *b) {
  return a->st_dev == b->st_dev &&
         a->st_ino == b->st_ino &&
         a->st_size == b->st_size &&
         ((a->st_mode & S_IFMT) == (b->st_mode & S_IFMT));
}

static int read_has_hello(int fd) {
  char buf[160];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  if (n < 0)
    return 0;
  buf[n] = '\0';
  return strstr(buf, "Hello, SBUnix!") != NULL;
}

static int check_relative_lookup(const char *label) {
  struct stat abs_st;
  struct stat rel_st;
  struct stat fd_st;
  int fails_before = g_fail;
  int fd;

  errno = 0;
  if (stat("/home/hello.c", &abs_st) != 0) {
    check("absolute stat /home/hello.c", 0);
    return 1;
  }
  check("absolute stat /home/hello.c", S_ISREG(abs_st.st_mode));

  errno = 0;
  if (stat("hello.c", &rel_st) != 0) {
    check(label, 0);
    return 1;
  }
  check(label, S_ISREG(rel_st.st_mode));

  check("relative stat matches absolute",
        same_file_stat(&abs_st, &rel_st));
  check("relative stat has mtime seconds",
        rel_st.st_mtim.tv_sec > 0);
  check("relative stat has zero mtime nsec",
        rel_st.st_mtim.tv_nsec == 0);

  errno = 0;
  fd = open("hello.c", O_RDONLY);
  check("relative open hello.c", fd >= 0);
  if (fd < 0)
    return 1;

  errno = 0;
  check("relative open reads source", read_has_hello(fd));
  errno = 0;
  check("relative fstat hello.c", fstat(fd, &fd_st) == 0);
  check("relative fstat matches stat", same_file_stat(&rel_st, &fd_st));
  check("relative fstat has mtime seconds", fd_st.st_mtim.tv_sec > 0);
  check("relative fstat has zero mtime nsec", fd_st.st_mtim.tv_nsec == 0);
  close(fd);

  return g_fail != fails_before;
}

static int wait_child_ok(pid_t pid, const char *label) {
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) {
    check(label, 0);
    return 0;
  }
  check(label, WIFEXITED(status) && WEXITSTATUS(status) == 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int exec_child(void) {
  char cwd[128];

  printf("=== cwd_tarfs_lookup exec child ===\n");
  errno = 0;
  check("exec child cwd readable", getcwd(cwd, sizeof(cwd)) != NULL);
  check("exec child cwd is /home", strcmp(cwd, "/home") == 0);
  check_relative_lookup("exec child relative stat hello.c");
  printf("=== cwd_tarfs_lookup exec child: %d passed, %d failed ===\n",
         g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
  char cwd[128];

  if (argc > 1 && strcmp(argv[1], "--exec-child") == 0)
    return exec_child();

  printf("=== cwd_tarfs_lookup ===\n");
  errno = 0;
  check("chdir /home", chdir("/home") == 0);
  errno = 0;
  check("getcwd after chdir", getcwd(cwd, sizeof(cwd)) != NULL);
  check("cwd is /home", strcmp(cwd, "/home") == 0);
  check_relative_lookup("same process relative stat hello.c");

  pid_t pid = fork();
  if (pid == 0) {
    int failed;
    g_pass = 0;
    g_fail = 0;
    printf("=== cwd_tarfs_lookup fork child ===\n");
    failed = check_relative_lookup("fork child relative stat hello.c");
    printf("=== cwd_tarfs_lookup fork child: %d passed, %d failed ===\n",
           g_pass, g_fail);
    _exit(failed ? 1 : 0);
  }
  check("fork child created", pid > 0);
  if (pid > 0)
    wait_child_ok(pid, "fork child relative lookup passed");

  pid = fork();
  if (pid == 0) {
    char *child_argv[] = {"/test/bin/cwd_tarfs_lookup", "--exec-child", NULL};
    char *child_envp[] = {NULL};
    execve(child_argv[0], child_argv, child_envp);
    _exit(127);
  }
  check("exec child created", pid > 0);
  if (pid > 0)
    wait_child_ok(pid, "exec child relative lookup passed");

  printf("=== cwd_tarfs_lookup: %d passed, %d failed ===\n",
         g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
