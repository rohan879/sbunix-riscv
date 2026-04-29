/*
 * pipekill_recycle -- killed pipe writers must leave blocked sleeps and free
 * their address-space pages.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/wait.h>

#define PRESSURE_MB 40
#define DATA_CHUNK 4096
#define MILESTONE_BYTES 4096
#define PIPE_CAPACITY_BYTES 65536
#define IDLE_POLLS_REQUIRED 3
#define IDLE_POLL_USEC 100000
#define OVERALL_TIMEOUT_SEC 180
#define PAGE_SIZE_BYTES 4096UL

static void fill_chunk(char *buf) {
  for (int i = 0; i < DATA_CHUNK; i++)
    buf[i] = (char)(0x40 + (i & 0x3f));
}

static ssize_t write_chunk(int fd, const char *buf, int use_writev) {
  if (!use_writev)
    return write(fd, buf, DATA_CHUNK);

  struct iovec iov[2];
  iov[0].iov_base = (void *)buf;
  iov[0].iov_len = DATA_CHUNK / 2;
  iov[1].iov_base = (void *)(buf + DATA_CHUNK / 2);
  iov[1].iov_len = DATA_CHUNK / 2;
  return writev(fd, iov, 2);
}

static void writer_child(int data_fd, int progress_fd, int use_writev) {
  char buf[DATA_CHUNK];
  uint64_t total = 0;
  uint64_t next_milestone = MILESTONE_BYTES;

  fill_chunk(buf);
  for (;;) {
    ssize_t n = write_chunk(data_fd, buf, use_writev);
    if (n <= 0)
      _exit(10);
    total += (uint64_t)n;
    if (total >= next_milestone) {
      uint64_t report = total;
      if (write(progress_fd, &report, sizeof(report)) != (ssize_t)sizeof(report))
        _exit(11);
      while (next_milestone <= total)
        next_milestone += MILESTONE_BYTES;
    }
  }
}

static int allocate_pressure(void) {
  size_t len = (size_t)PRESSURE_MB * 1024UL * 1024UL;
  unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    return 0;

  for (size_t off = 0; off < len; off += PAGE_SIZE_BYTES)
    p[off] = (unsigned char)(0x21 + ((off / PAGE_SIZE_BYTES) & 0x7f));

  for (size_t off = 0; off < len; off += PAGE_SIZE_BYTES) {
    unsigned char want = (unsigned char)(0x21 + ((off / PAGE_SIZE_BYTES) & 0x7f));
    if (p[off] != want) {
      munmap(p, len);
      return 0;
    }
  }

  munmap(p, len);
  return 1;
}

static int wait_until_blocked(int progress_fd, uint64_t *last_total) {
  int idle_polls = 0;
  *last_total = 0;

  fcntl(progress_fd, F_SETFL, O_NONBLOCK);
  while (idle_polls < IDLE_POLLS_REQUIRED) {
    uint64_t report = 0;
    ssize_t n = read(progress_fd, &report, sizeof(report));
    if (n == (ssize_t)sizeof(report)) {
      *last_total = report;
      idle_polls = 0;
      continue;
    }
    if (n > 0)
      continue;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      return 0;

    if (*last_total >= PIPE_CAPACITY_BYTES)
      idle_polls++;
    usleep(IDLE_POLL_USEC);
  }

  return *last_total >= PIPE_CAPACITY_BYTES;
}

static int run_case(int use_writev) {
  int data_pipe[2];
  int progress_pipe[2];
  if (pipe(data_pipe) != 0)
    return 1;
  if (pipe(progress_pipe) != 0) {
    close(data_pipe[0]);
    close(data_pipe[1]);
    return 2;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(data_pipe[0]);
    close(data_pipe[1]);
    close(progress_pipe[0]);
    close(progress_pipe[1]);
    return 3;
  }

  if (pid == 0) {
    close(data_pipe[0]);
    close(progress_pipe[0]);
    writer_child(data_pipe[1], progress_pipe[1], use_writev);
  }

  close(data_pipe[1]);
  close(progress_pipe[1]);

  uint64_t last_total = 0;
  if (!wait_until_blocked(progress_pipe[0], &last_total)) {
    kill(pid, SIGKILL);
    waitpid(pid, 0, 0);
    close(data_pipe[0]);
    close(progress_pipe[0]);
    return 4;
  }

  if (kill(pid, SIGKILL) != 0) {
    close(data_pipe[0]);
    close(progress_pipe[0]);
    return 5;
  }

  int status = 0;
  if (waitpid(pid, &status, 0) != pid) {
    close(data_pipe[0]);
    close(progress_pipe[0]);
    return 6;
  }

  close(progress_pipe[0]);
  close(data_pipe[0]);

  if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
    return 7;
  if (!allocate_pressure())
    return 8;

  return 0;
}

int main(int argc, char **argv) {
  int use_writev = 0;
  const char *mode = "write";

  alarm(OVERALL_TIMEOUT_SEC);
  if (argc > 1) {
    if (strcmp(argv[1], "writev") == 0) {
      use_writev = 1;
      mode = "writev";
    } else if (strcmp(argv[1], "write") == 0) {
      use_writev = 0;
      mode = "write";
    } else {
      printf("[FAIL] pipekill_recycle: unknown mode %s\n", argv[1]);
      return 1;
    }
  }

  int rc = run_case(use_writev);
  if (rc == 0) {
    printf("[PASS] pipekill_recycle: %s\n", mode);
    return 0;
  }

  printf("[FAIL] pipekill_recycle: %s rc=%d\n", mode, rc);
  return 1;
}
