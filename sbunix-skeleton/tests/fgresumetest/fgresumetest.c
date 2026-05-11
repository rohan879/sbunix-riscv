/*
 * fgresumetest -- foreground resume and terminal handoff coverage.
 *
 * This test drives the same kernel/libc contract used by a shell fg command:
 * hand the tty to a stopped job's process group, send SIGCONT to that whole
 * group, observe WCONTINUED, let the resumed job run in the foreground, then
 * wait for exit and reclaim the tty.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_pass = 0;
static int g_fail = 0;
static int g_skip = 0;

#define CHECK(label, cond) do { \
  if (cond) { \
    printf("[PASS] fgresume: %s\n", label); \
    g_pass++; \
  } else { \
    printf("[FAIL] fgresume: %s\n", label); \
    g_fail++; \
  } \
} while (0)

#define SKIP(label, reason) do { \
  printf("[SKIP] fgresume: %s (%s)\n", label, reason); \
  g_skip++; \
} while (0)

static volatile sig_atomic_t got_cont = 0;

static void cont_handler(int sig)
{
  (void)sig;
  got_cont = 1;
}

static int wait_for_byte(int fd, char want, int attempts)
{
  for (int i = 0; i < attempts; i++) {
    char ch = 0;
    ssize_t n = read(fd, &ch, 1);
    if (n == 1) {
      if (ch == want)
        return 1;
      continue;
    }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      return 0;
    usleep(10000);
  }
  return 0;
}

static int read_full_nonblock(int fd, unsigned char *buf, size_t len,
                              size_t *done)
{
  while (*done < len) {
    ssize_t n = read(fd, buf + *done, len - *done);
    if (n > 0) {
      *done += (size_t)n;
      continue;
    }
    if (n == 0)
      return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return 0;
    return 0;
  }
  return 1;
}

static int wait_for_pid(int fd, pid_t *pid_out, size_t *done, int attempts)
{
  unsigned char *p = (unsigned char *)pid_out;

  for (int i = 0; i < attempts; i++) {
    if (read_full_nonblock(fd, p, sizeof(*pid_out), done))
      return 1;
    usleep(10000);
  }
  return 0;
}

static pid_t wait_status(pid_t pid, int *status, int options, int attempts)
{
  for (int i = 0; i < attempts; i++) {
    pid_t got = waitpid(pid, status, options | WNOHANG);
    if (got != 0)
      return got;
    usleep(10000);
  }
  errno = ETIMEDOUT;
  return -1;
}

struct fg_report {
  int controller_owns_tty;
  int child_pgrp;
  int child_ready;
  int stop_handoff;
  int stopped;
  int reclaim_after_stop;
  int cont_handoff;
  int continued;
  int ran_foreground;
  int exit_reaped;
  int reclaim_after_exit;
};

static int wait_for_report(int fd, struct fg_report *report, size_t *done,
                           int attempts)
{
  unsigned char *p = (unsigned char *)report;

  for (int i = 0; i < attempts; i++) {
    if (read_full_nonblock(fd, p, sizeof(*report), done))
      return 1;
    usleep(10000);
  }
  return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
  const unsigned char *p = buf;
  size_t done = 0;

  while (done < len) {
    ssize_t n = write(fd, p + done, len - done);
    if (n <= 0)
      return -1;
    done += (size_t)n;
  }
  return 0;
}

static void child_job(int progress_fd, int release_fd)
{
  struct sigaction sa;

  setpgid(0, 0);
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = cont_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGCONT, &sa, NULL);

  write(progress_fd, "R", 1);

  while (!got_cont)
    usleep(10000);

  pid_t fg = tcgetpgrp(STDIN_FILENO);
  if (fg == getpgrp())
    write(progress_fd, "F", 1);
  else
    write(progress_fd, "B", 1);

  char release = 0;
  while (read(release_fd, &release, 1) != 1)
    usleep(10000);

  _exit((got_cont && fg == getpgrp()) ? 0 : 1);
}

static void controller_main(int report_fd, int job_fd)
{
  struct fg_report r;
  int progress[2];
  int release[2];
  int status = 0;
  pid_t child = -1;
  pid_t controller_pgrp;
  struct sigaction ignore_ttou;
  struct sigaction old_ttou;

  memset(&r, 0, sizeof(r));
  controller_pgrp = getpgrp();

  memset(&ignore_ttou, 0, sizeof(ignore_ttou));
  ignore_ttou.sa_handler = SIG_IGN;
  sigemptyset(&ignore_ttou.sa_mask);
  sigaction(SIGTTOU, &ignore_ttou, &old_ttou);

  r.controller_owns_tty = (tcgetpgrp(STDIN_FILENO) == controller_pgrp);

  if (pipe2(progress, O_NONBLOCK) != 0 || pipe(release) != 0) {
    write_full(report_fd, &r, sizeof(r));
    _exit(3);
  }

  child = fork();
  if (child == 0) {
    close(progress[0]);
    close(release[1]);
    child_job(progress[1], release[0]);
  }

  close(progress[1]);
  close(release[0]);
  if (child < 0) {
    close(progress[0]);
    close(release[1]);
    sigaction(SIGTTOU, &old_ttou, NULL);
    write_full(report_fd, &r, sizeof(r));
    _exit(4);
  }

  setpgid(child, child);
  write_full(job_fd, &child, sizeof(child));
  r.child_pgrp = (getpgid(child) == child);
  r.child_ready = wait_for_byte(progress[0], 'R', 200);

  r.stop_handoff = (r.child_ready &&
                    tcsetpgrp(STDIN_FILENO, child) == 0 &&
                    tcgetpgrp(STDIN_FILENO) == child);
  r.stopped = (r.stop_handoff &&
               kill(-child, SIGTSTP) == 0 &&
               wait_status(-child, &status, WUNTRACED, 200) == child &&
               WIFSTOPPED(status) && WSTOPSIG(status) == SIGTSTP);
  r.reclaim_after_stop = (tcsetpgrp(STDIN_FILENO, controller_pgrp) == 0 &&
                          tcgetpgrp(STDIN_FILENO) == controller_pgrp);

  r.cont_handoff = (r.stopped && r.reclaim_after_stop &&
                    tcsetpgrp(STDIN_FILENO, child) == 0 &&
                    tcgetpgrp(STDIN_FILENO) == child);
  r.continued = (r.cont_handoff &&
                 kill(-child, SIGCONT) == 0 &&
                 wait_status(-child, &status, WCONTINUED, 200) == child &&
                 WIFCONTINUED(status));
  r.ran_foreground = (r.continued && wait_for_byte(progress[0], 'F', 200));

  write(release[1], "X", 1);
  r.exit_reaped = (wait_status(-child, &status, 0, 200) == child &&
                   WIFEXITED(status) && WEXITSTATUS(status) == 0);
  r.reclaim_after_exit = (tcsetpgrp(STDIN_FILENO, controller_pgrp) == 0 &&
                          tcgetpgrp(STDIN_FILENO) == controller_pgrp);
  if (!r.exit_reaped) {
    /* TEST: Failed resume paths must not leave the controller stuck in a
     * blocking cleanup wait when this diagnostic runs in the full suite. */
    kill(-child, SIGCONT);
    kill(-child, SIGKILL);
    wait_status(child, &status, 0, 200);
  }

  close(progress[0]);
  close(release[1]);
  sigaction(SIGTTOU, &old_ttou, NULL);
  write_full(report_fd, &r, sizeof(r));

  _exit(r.controller_owns_tty && r.child_pgrp && r.child_ready &&
        r.stop_handoff && r.stopped && r.reclaim_after_stop &&
        r.cont_handoff && r.continued && r.ran_foreground &&
        r.exit_reaped && r.reclaim_after_exit ? 0 : 1);
}

int main(void)
{
  struct fg_report r;
  int report_pipe[2];
  int job_pipe[2];
  int got_report = 0;
  int supervisor_reclaim = 0;
  size_t report_done = 0;
  size_t job_pgrp_done = 0;
  pid_t controller;
  pid_t supervisor_pgrp;
  pid_t job_pgrp = -1;
  struct sigaction ignore_ttou;
  struct sigaction old_ttou;

  memset(&r, 0, sizeof(r));

  printf("=== fgresumetest: foreground resume ===\n");

  supervisor_pgrp = getpgrp();
  if (tcgetpgrp(STDIN_FILENO) < 0) {
    SKIP("controlling tty present", "tcgetpgrp failed");
    printf("=== fgresumetest: %d passed, %d failed, %d skipped ===\n",
           g_pass, g_fail, g_skip);
    return 0;
  }

  memset(&ignore_ttou, 0, sizeof(ignore_ttou));
  ignore_ttou.sa_handler = SIG_IGN;
  sigemptyset(&ignore_ttou.sa_mask);
  sigaction(SIGTTOU, &ignore_ttou, &old_ttou);

  CHECK("supervisor owns tty before job-control sequence",
        tcsetpgrp(STDIN_FILENO, supervisor_pgrp) == 0 &&
        tcgetpgrp(STDIN_FILENO) == supervisor_pgrp);

  if (pipe2(report_pipe, O_NONBLOCK) != 0 ||
      pipe2(job_pipe, O_NONBLOCK) != 0) {
    CHECK("pipe setup", 0);
    sigaction(SIGTTOU, &old_ttou, NULL);
    return 1;
  }

  controller = fork();
  if (controller == 0) {
    close(report_pipe[0]);
    close(job_pipe[0]);
    controller_main(report_pipe[1], job_pipe[1]);
  }

  close(report_pipe[1]);
  close(job_pipe[1]);
  if (controller < 0) {
    CHECK("fork controller", 0);
    close(report_pipe[0]);
    close(job_pipe[0]);
    sigaction(SIGTTOU, &old_ttou, NULL);
    return 1;
  }

  /* TEST: The supervisor uses pipes, not waitpid, for its watchdog.  Partial
   * nonblocking reads are accumulated across polls so report/job-pgid bytes
   * cannot be discarded before cleanup decides which process group to kill. */
  for (int i = 0; i < 1500 && !got_report; i++) {
    got_report = wait_for_report(report_pipe[0], &r, &report_done, 1);
    if (job_pgrp <= 0)
      wait_for_pid(job_pipe[0], &job_pgrp, &job_pgrp_done, 1);
  }
  if (job_pgrp <= 0)
    wait_for_pid(job_pipe[0], &job_pgrp, &job_pgrp_done, 100);

  supervisor_reclaim = (tcsetpgrp(STDIN_FILENO, supervisor_pgrp) == 0 &&
                        tcgetpgrp(STDIN_FILENO) == supervisor_pgrp);

  if (!got_report) {
    if (job_pgrp > 0) {
      kill(-job_pgrp, SIGCONT);
      kill(-job_pgrp, SIGKILL);
    }
    /* TEST: Keep cleanup best-effort and bounded so a foreground-resume
     * regression is reported as a failed check instead of pinning CI. */
    kill(controller, SIGCONT);
    kill(controller, SIGKILL);
    supervisor_reclaim = (tcsetpgrp(STDIN_FILENO, supervisor_pgrp) == 0 &&
                          tcgetpgrp(STDIN_FILENO) == supervisor_pgrp) &&
                         supervisor_reclaim;
  }

  close(report_pipe[0]);
  close(job_pipe[0]);
  sigaction(SIGTTOU, &old_ttou, NULL);

  CHECK("controller returned before watchdog", got_report);
  CHECK("supervisor reclaimed tty after controller", supervisor_reclaim);
  CHECK("controller owns tty before job handoff", got_report && r.controller_owns_tty);
  CHECK("child became process-group leader", got_report && r.child_pgrp);
  CHECK("child reported initial progress", got_report && r.child_ready);
  CHECK("tty handed to child process group", got_report && r.stop_handoff);
  CHECK("foreground group stopped", got_report && r.stopped);
  CHECK("controller reclaimed tty after stop", got_report && r.reclaim_after_stop);
  CHECK("tty handed back before continue", got_report && r.cont_handoff);
  CHECK("continued whole foreground group", got_report && r.continued);
  CHECK("continued job ran with tty foreground ownership",
        got_report && r.ran_foreground);
  CHECK("foreground job exited and was reaped", got_report && r.exit_reaped);
  CHECK("controller reclaimed tty after exit", got_report && r.reclaim_after_exit);

  printf("=== fgresumetest: %d passed, %d failed, %d skipped ===\n",
         g_pass, g_fail, g_skip);
  return g_fail > 0 ? 1 : 0;
}
