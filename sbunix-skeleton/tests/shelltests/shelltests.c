/*
 * shelltests — shell redirect and script execution tests
 *
 * Tests output redirect, pipe, input redirect, and script execution
 * via fork+exec of /bin/sbush -c.
 *
 * Run from the shell:  shelltests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] shelltests: %s\n", label); g_pass++; } \
    else      { printf("[FAIL] shelltests: %s\n", label); g_fail++; } \
} while (0)

/* Run a shell command via sbush -c and capture stdout into buf.
 * Returns exit status, or -1 on fork/exec failure. */
static int run_shell(const char *cmd, char *buf, int bufsz) {
  int pipefd[2];
  if (pipe(pipefd) < 0)
    return -1;

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }
  if (pid == 0) {
    /* child: redirect stdout to pipe, exec sbush */
    close(pipefd[0]);
    dup2(pipefd[1], 1);
    close(pipefd[1]);
    char *argv[] = {"/bin/sbush", "-c", (char *)cmd, NULL};
    execve("/bin/sbush", argv, NULL);
    _exit(127);
  }
  /* parent: read pipe */
  close(pipefd[1]);
  int total = 0;
  while (total < bufsz - 1) {
    int n = read(pipefd[0], buf + total, bufsz - 1 - total);
    if (n <= 0) break;
    total += n;
  }
  buf[total] = '\0';
  close(pipefd[0]);

  int status;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(void) {
  printf("=== shelltests: shell redirect and script test ===\n");

  char buf[512];

  /* Smoke check: run echo hello via sbush -c */
  {
    int rc = run_shell("echo hello", buf, sizeof(buf));
    /* Trim trailing newline for comparison */
    int len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    int smoke_ok = (rc == 0 && strcmp(buf, "hello") == 0);
    CHECK("smoke: echo hello via sbush -c", smoke_ok);
    if (!smoke_ok) {
      printf("shelltests: smoke check failed (rc=%d buf='%s'), skipping remaining tests\n", rc, buf);
      printf("shelltests: %d passed, %d failed\n", g_pass, g_fail);
      return (g_fail > 0) ? 1 : 0;
    }
  }

  /* Test 1: Output redirect: echo X > file */
  {
    /* Clean up any leftover */
    unlink("/tmp/shelltests_out");
    int rc = run_shell("echo redirect_test > /tmp/shelltests_out", buf, sizeof(buf));
    CHECK("output redirect: command succeeds", rc == 0);

    /* Read the file */
    int fd = open("/tmp/shelltests_out", O_RDONLY);
    CHECK("output redirect: file created", fd >= 0);
    if (fd >= 0) {
      char fbuf[128];
      int n = read(fd, fbuf, sizeof(fbuf) - 1);
      if (n > 0) {
        fbuf[n] = '\0';
        /* Trim newline */
        if (n > 0 && fbuf[n - 1] == '\n') fbuf[n - 1] = '\0';
        CHECK("output redirect: file content correct",
              strcmp(fbuf, "redirect_test") == 0);
      } else {
        CHECK("output redirect: file content correct", 0);
      }
      close(fd);
    }
    unlink("/tmp/shelltests_out");
  }

  /* Test 2: Pipe: echo X | cat */
  {
    int rc = run_shell("echo pipe_test | cat", buf, sizeof(buf));
    int len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    CHECK("pipe: echo X | cat works",
          rc == 0 && strcmp(buf, "pipe_test") == 0);
  }

  /* Test 3: Input redirect: cat < file */
  {
    /* Write test file */
    int fd = open("/tmp/shelltests_in", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
      write(fd, "input_test\n", 11);
      close(fd);
    }
    int rc = run_shell("cat < /tmp/shelltests_in", buf, sizeof(buf));
    int len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    CHECK("input redirect: cat < file works",
          rc == 0 && strcmp(buf, "input_test") == 0);
    unlink("/tmp/shelltests_in");
  }

  /* Test 4: Script execution */
  {
    /* Write a script */
    int fd = open("/tmp/shelltests_script", O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd >= 0) {
      const char *script = "#!/bin/sbush\necho script_output\n";
      write(fd, script, strlen(script));
      close(fd);
    }
    int rc = run_shell("/tmp/shelltests_script", buf, sizeof(buf));
    int len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    CHECK("script execution: output correct",
          rc == 0 && strcmp(buf, "script_output") == 0);
    unlink("/tmp/shelltests_script");
  }

  printf("shelltests: %d passed, %d failed\n", g_pass, g_fail);
  return (g_fail > 0) ? 1 : 0;
}
