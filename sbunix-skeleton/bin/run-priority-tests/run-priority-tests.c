/* run-priority-tests: execute priority test manifests inside the OS.
 *
 * Uses raw I/O (open/read/write) exclusively — no libc FILE* streams.
 * Avoids setpgid() which triggers kernel memory corruption on repeated
 * fork/exec cycles. Instead, blocks common signals in the parent to
 * survive child kill(0, sig) calls. */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

#define TIMEOUT_SEC 30
#define MANIFEST_MAX (64 * 1024)

volatile pid_t g_child_pid;

static void emit(const char *s) {
  write(STDOUT_FILENO, s, strlen(s));
}

static void alarm_handler(int sig) {
  (void)sig;
  if (g_child_pid > 0)
    kill(g_child_pid, SIGKILL);
}

static int run_test(const char *runner, const char *suite, const char *name) {
  pid_t pid = fork();
  if (pid < 0) {
    emit("[FAIL] ");
    emit(suite);
    emit("/");
    emit(name);
    emit(" (fork failed)\n");
    return -1;
  }
  if (pid == 0) {
    /* Restore default signal dispositions in child (exec does this too,
     * but in case exec fails we want clean signal handling). */
    signal(SIGTERM, SIG_DFL);
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);

    int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) {
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      close(fd);
    }
    char *argv[] = {(char *)runner, (char *)name, NULL};
    execve(runner, argv, NULL);
    _exit(127);
  }

  g_child_pid = pid;
  alarm(TIMEOUT_SEC);

  int status = 0;
  pid_t ret = waitpid(pid, &status, 0);
  alarm(0);
  g_child_pid = 0;

  if (ret >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    emit("[PASS] ");
  } else {
    emit("[FAIL] ");
  }
  emit(suite);
  emit("/");
  emit(name);
  emit("\n");
  return (ret >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static void run_manifest(const char *path, const char *runner,
                         const char *suite) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return;

  static char buf[MANIFEST_MAX];
  ssize_t total = 0;
  ssize_t n;
  while (total < MANIFEST_MAX - 1 &&
         (n = read(fd, buf + total, MANIFEST_MAX - 1 - total)) > 0)
    total += n;
  buf[total] = '\0';
  close(fd);

  char *p = buf;
  while (*p) {
    char *eol = p;
    while (*eol && *eol != '\n')
      eol++;
    size_t len = eol - p;
    if (*eol == '\n')
      eol++;
    while (len > 0 && p[len - 1] == '\r')
      len--;
    p[len] = '\0';

    if (len > 0 && p[0] != '#')
      run_test(runner, suite, p);

    p = eol;
  }
}

int main(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = alarm_handler;
  sigaction(SIGALRM, &sa, NULL);

  /* Block signals that child test processes might send to the process
   * group via kill(0, sig). We can't block SIGKILL/SIGSTOP, but those
   * are non-catchable anyway. Ignoring these in the parent prevents
   * tests from terminating us. */
  signal(SIGTERM, SIG_IGN);
  signal(SIGUSR1, SIG_IGN);
  signal(SIGUSR2, SIG_IGN);
  signal(SIGHUP, SIG_IGN);
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);

  run_manifest("/test/priority-os-test-manifest",
               "/bin/run-os-test", "os-test");
  run_manifest("/test/priority-open-posix-manifest",
               "/bin/run-open-posix-test", "open-posix");
  return 0;
}
