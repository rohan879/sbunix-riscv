/*
 * p1edgetest.c -- targeted P1 syscall and process edge regressions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syscall.h>

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

static int g_pass;
static int g_fail;

#define CHECK(label, cond) do { \
  if (cond) { printf("[PASS] p1edge: %s\n", label); g_pass++; } \
  else { printf("[FAIL] p1edge: %s\n", label); g_fail++; } \
} while (0)

static int syscall_to_int(long ret) {
  if (ret < 0 && ret >= -4095) {
    errno = (int)-ret;
    return -1;
  }
  return (int)ret;
}

static int child_exited(pid_t pid, int want) {
  int status;
  if (waitpid(pid, &status, 0) != pid)
    return 0;
  return WIFEXITED(status) && WEXITSTATUS(status) == want;
}

static const char *self_path(const char *argv0) {
  if (argv0 && argv0[0] == '/')
    return argv0;
  return "/test/bin/p1edgetest";
}

static int fdcheck_main(int argc, char **argv) {
  if (argc < 4)
    return 90;
  int closed_fd = atoi(argv[2]);
  int open_fd = atoi(argv[3]);

  errno = 0;
  int closed = fcntl(closed_fd, F_GETFD);
  int closed_errno = errno;
  errno = 0;
  int open = fcntl(open_fd, F_GETFD);
  return (closed == -1 && closed_errno == EBADF && open >= 0) ? 0 : 91;
}

static volatile sig_atomic_t got_chld;

static void chld_handler(int sig) {
  (void)sig;
  got_chld = 1;
}

static void test_signal_mask_inheritance(void) {
  sigset_t set;
  sigset_t oldset;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  if (sigprocmask(SIG_BLOCK, &set, &oldset) != 0) {
    CHECK("signal mask setup", 0);
    return;
  }

  pid_t pid = fork();
  if (pid == 0) {
    sigset_t cur;
    sigemptyset(&cur);
    if (sigprocmask(SIG_SETMASK, NULL, &cur) != 0)
      _exit(2);
    _exit(sigismember(&cur, SIGUSR1) == 1 ? 0 : 1);
  }

  int ok = child_exited(pid, 0);
  sigprocmask(SIG_SETMASK, &oldset, NULL);
  CHECK("signal mask inherited across fork", ok);
}

static void test_sigchld_handler_delivery(void) {
  struct sigaction sa;
  struct sigaction oldsa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = chld_handler;
  sigemptyset(&sa.sa_mask);
  got_chld = 0;
  if (sigaction(SIGCHLD, &sa, &oldsa) != 0) {
    CHECK("SIGCHLD handler install", 0);
    return;
  }

  pid_t pid = fork();
  if (pid == 0)
    _exit(0);

  for (int i = 0; i < 100 && !got_chld; i++)
    usleep(1000);

  int ok = child_exited(pid, 0) && got_chld;
  sigaction(SIGCHLD, &oldsa, NULL);
  CHECK("SIGCHLD handler delivered", ok);
}

static void test_pipe_nonblocking(void) {
  int pfd[2];
  if (pipe(pfd) != 0) {
    CHECK("pipe nonblocking setup", 0);
    return;
  }

  int rflags = fcntl(pfd[0], F_GETFL);
  int wflags = fcntl(pfd[1], F_GETFL);
  fcntl(pfd[0], F_SETFL, rflags | O_NONBLOCK);
  fcntl(pfd[1], F_SETFL, wflags | O_NONBLOCK);

  char c;
  errno = 0;
  ssize_t nr = read(pfd[0], &c, 1);
  int read_errno = errno;

  char buf[4096];
  memset(buf, 'x', sizeof(buf));
  int saw_eagain = 0;
  int total = 0;
  for (;;) {
    errno = 0;
    ssize_t nw = write(pfd[1], buf, sizeof(buf));
    if (nw < 0) {
      saw_eagain = (errno == EAGAIN || errno == EWOULDBLOCK);
      break;
    }
    if (nw == 0)
      break;
    total += (int)nw;
    if (total > 200000)
      break;
  }

  close(pfd[0]);
  close(pfd[1]);
  CHECK("pipe read O_NONBLOCK returns EAGAIN",
        nr == -1 && (read_errno == EAGAIN || read_errno == EWOULDBLOCK));
  CHECK("pipe write O_NONBLOCK returns EAGAIN", total > 0 && saw_eagain);
}

static void test_poll_pipe(void) {
  int pfd[2];
  if (pipe(pfd) != 0) {
    CHECK("poll pipe setup", 0);
    return;
  }

  struct pollfd p;
  p.fd = pfd[0];
  p.events = POLLIN;
  p.revents = 0;
  int before = poll(&p, 1, 0);
  write(pfd[1], "z", 1);
  p.revents = 0;
  int after = poll(&p, 1, 0);
  close(pfd[0]);
  close(pfd[1]);
  CHECK("poll empty pipe timeout", before == 0);
  CHECK("poll readable pipe", after == 1 && (p.revents & POLLIN));
}

static void test_cloexec_isolation(const char *argv0) {
  int pfd[2];
  if (pipe(pfd) != 0) {
    CHECK("FD_CLOEXEC setup", 0);
    return;
  }

  fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
  pid_t pid = fork();
  if (pid == 0) {
    char closed[16];
    char openfd[16];
    snprintf(closed, sizeof(closed), "%d", pfd[0]);
    snprintf(openfd, sizeof(openfd), "%d", pfd[1]);
    char *args[] = {
      (char *)self_path(argv0), "--fdcheck", closed, openfd, NULL
    };
    char *env[] = { NULL };
    execve(self_path(argv0), args, env);
    _exit(127);
  }

  close(pfd[0]);
  close(pfd[1]);
  CHECK("FD_CLOEXEC closes only marked fd", child_exited(pid, 0));
}

static void test_proc_fd_readlink(void) {
  const char *path = "/tmp/p1_procfd";
  unlink(path);
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd < 0) {
    CHECK("proc fd readlink setup", 0);
    return;
  }

  char procpath[64];
  char link[128];
  snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", fd);
  int n = readlink(procpath, link, sizeof(link) - 1);
  if (n > 0)
    link[n] = '\0';
  close(fd);
  unlink(path);
  CHECK("proc fd readlink returns path",
        n > 0 && strstr(link, "/tmp/p1_procfd") != NULL);
}

static void test_open_flags_ext2(void) {
  const char *target = "/mnt/p1_link_target";
  const char *linkp = "/mnt/p1_link";
  unlink(linkp);
  unlink(target);

  int fd = open(target, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd >= 0) {
    write(fd, "x", 1);
    close(fd);
  }
  symlink(target, linkp);

  errno = 0;
  int lfd = open(linkp, O_RDONLY | O_NOFOLLOW);
  int nofollow_errno = errno;
  if (lfd >= 0)
    close(lfd);

  errno = 0;
  int xfd = open(target, O_CREAT | O_EXCL | O_RDWR, 0644);
  int excl_errno = errno;
  if (xfd >= 0)
    close(xfd);

  unlink(linkp);
  unlink(target);
  CHECK("O_NOFOLLOW rejects final ext2 symlink",
        lfd == -1 && nofollow_errno == ELOOP);
  CHECK("O_EXCL rejects existing ext2 file",
        xfd == -1 && excl_errno == EEXIST);
}

static void test_at_empty_path(void) {
  const char *path = "/tmp/p1_empty_path";
  unlink(path);
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd < 0) {
    CHECK("AT_EMPTY_PATH setup", 0);
    return;
  }
  write(fd, "abc", 3);

  struct stat st1;
  struct stat st2;
  int r1 = fstat(fd, &st1);
  errno = 0;
  int r2 = fstatat(fd, "", &st2, AT_EMPTY_PATH);
  int ok = r1 == 0 && r2 == 0 && st1.st_size == st2.st_size;
  close(fd);
  unlink(path);
  CHECK("fstatat AT_EMPTY_PATH stats fd", ok);
}

static void test_renameat2_zero_flags(void) {
  const char *oldp = "/tmp/p1_rename_old";
  const char *newp = "/tmp/p1_rename_new";
  unlink(oldp);
  unlink(newp);
  int fd = open(oldp, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd >= 0) {
    write(fd, "r", 1);
    close(fd);
  }

  errno = 0;
  int rr = syscall_to_int(syscall(SYS_renameat2, AT_FDCWD, oldp,
                                  AT_FDCWD, newp, 0));
  int nfd = open(newp, O_RDONLY);
  int ofd = open(oldp, O_RDONLY);
  if (nfd >= 0)
    close(nfd);
  if (ofd >= 0)
    close(ofd);
  unlink(newp);
  unlink(oldp);
  CHECK("renameat2 zero flags renames", rr == 0 && nfd >= 0 && ofd == -1);
}

static void test_renameat2_rejects_flags(void) {
  const char *oldp = "/tmp/p1_rename_flag_old";
  const char *newp = "/tmp/p1_rename_flag_new";
  unlink(oldp);
  unlink(newp);
  int fd = open(oldp, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd >= 0) {
    write(fd, "r", 1);
    close(fd);
  }

  errno = 0;
  int rr = syscall_to_int(syscall(SYS_renameat2, AT_FDCWD, oldp,
                                  AT_FDCWD, newp, 1));
  int saved_errno = errno;
  int old_exists = access(oldp, F_OK) == 0;
  int new_exists = access(newp, F_OK) == 0;
  unlink(oldp);
  unlink(newp);
  CHECK("renameat2 rejects unsupported flags",
        rr == -1 && saved_errno == EINVAL && old_exists && !new_exists);
}

static pid_t spawn_group_child(pid_t pgid, int ready_write) {
  pid_t pid = fork();
  if (pid == 0) {
    pid_t self = getpid();
    pid_t want = pgid == 0 ? self : pgid;
    setpgid(0, want);
    write(ready_write, "R", 1);
    close(ready_write);
    for (;;)
      pause();
  }
  return pid;
}

static void test_stopped_process_group_wait(void) {
  int ready[2];
  if (pipe(ready) != 0) {
    CHECK("stopped pgrp setup", 0);
    return;
  }

  pid_t leader = spawn_group_child(0, ready[1]);
  char c;
  read(ready[0], &c, 1);
  setpgid(leader, leader);

  pid_t member = spawn_group_child(leader, ready[1]);
  read(ready[0], &c, 1);
  setpgid(member, leader);
  close(ready[0]);
  close(ready[1]);

  kill(-leader, SIGSTOP);
  int stopped = 0;
  for (int i = 0; i < 2; i++) {
    int status;
    pid_t got = waitpid(-leader, &status, WUNTRACED);
    if ((got == leader || got == member) && WIFSTOPPED(status))
      stopped++;
  }

  kill(-leader, SIGCONT);
  kill(-leader, SIGKILL);
  int reaped = 0;
  for (int i = 0; i < 2; i++) {
    int status;
    pid_t got = waitpid(-leader, &status, 0);
    if ((got == leader || got == member) &&
        (WIFSIGNALED(status) || WIFEXITED(status)))
      reaped++;
  }
  CHECK("waitpid observes stopped process group", stopped == 2 && reaped == 2);
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--fdcheck") == 0)
    return fdcheck_main(argc, argv);

  test_signal_mask_inheritance();
  test_sigchld_handler_delivery();
  test_pipe_nonblocking();
  test_poll_pipe();
  test_cloexec_isolation(argv[0]);
  test_proc_fd_readlink();
  test_open_flags_ext2();
  test_at_empty_path();
  test_renameat2_zero_flags();
  test_renameat2_rejects_flags();
  test_stopped_process_group_wait();

  printf("p1edgetest: passed=%d failed=%d\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
