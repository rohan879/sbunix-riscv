/*
 * largeworkloadtest -- compiler-like user-space stress coverage.
 *
 * The workload runs in a child while the parent watches progress and reaps it.
 * It creates and rereads many small input files, grows a multi-megabyte heap,
 * touches a private anonymous mapping, and writes an executable output file in
 * many small chunks on /mnt.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define INPUT_COUNT 40
#define INPUT_SIZE 4096
#define HEAP_CHUNKS 96
#define HEAP_CHUNK_SIZE 65536
#define MMAP_SIZE (2 * 1024 * 1024)
#define OUTPUT_CHUNKS 512
#define OUTPUT_CHUNK_SIZE 512

static int g_pass = 0;
static int g_fail = 0;
static unsigned char io_buf[INPUT_SIZE];
static void *heap_chunks[HEAP_CHUNKS];

#define CHECK(label, cond) do { \
  if (cond) { \
    printf("[PASS] largeworkload: %s\n", label); \
    g_pass++; \
  } else { \
    printf("[FAIL] largeworkload: %s\n", label); \
    g_fail++; \
  } \
} while (0)

static void progress(int fd, char phase)
{
  write(fd, &phase, 1);
}

static int ensure_dir(const char *path)
{
  if (mkdir(path, 0755) == 0)
    return 0;
  return errno == EEXIST ? 0 : -1;
}

static void input_path(int i, char *buf, int len)
{
  snprintf(buf, len, "/mnt/largeworkload/input-%02d.h", i);
}

static int write_all(int fd, const void *buf, size_t len)
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

static int create_inputs(void)
{
  char path[64];

  if (ensure_dir("/mnt/largeworkload") != 0)
    return -1;

  for (int i = 0; i < INPUT_COUNT; i++) {
    input_path(i, path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
      return -1;
    for (int j = 0; j < INPUT_SIZE; j++)
      io_buf[j] = (unsigned char)('A' + ((i + j) % 26));
    if (write_all(fd, io_buf, INPUT_SIZE) != 0) {
      close(fd);
      return -1;
    }
    close(fd);
  }

  return 0;
}

static int read_inputs(uint32_t *checksum_out)
{
  uint32_t checksum = 0;
  char path[64];

  for (int pass = 0; pass < 3; pass++) {
    for (int i = 0; i < INPUT_COUNT; i++) {
      input_path(i, path, sizeof(path));
      int fd = open(path, O_RDONLY);
      if (fd < 0)
        return -1;
      for (;;) {
        ssize_t n = read(fd, io_buf, sizeof(io_buf));
        if (n < 0) {
          close(fd);
          return -1;
        }
        if (n == 0)
          break;
        for (ssize_t j = 0; j < n; j++)
          checksum = (checksum * 33u) ^ io_buf[j];
      }
      close(fd);
    }
  }

  *checksum_out = checksum;
  return checksum != 0 ? 0 : -1;
}

static int grow_heap(uint32_t seed)
{
  for (int i = 0; i < HEAP_CHUNKS; i++) {
    unsigned char *p = malloc(HEAP_CHUNK_SIZE);
    if (!p)
      return -1;
    heap_chunks[i] = p;
    for (size_t off = 0; off < HEAP_CHUNK_SIZE; off += 4096)
      p[off] = (unsigned char)(seed + i + off);
    p[HEAP_CHUNK_SIZE - 1] = (unsigned char)(seed + i);
  }
  return 0;
}

static int touch_mapping(uint32_t seed)
{
  unsigned char *p = mmap(NULL, MMAP_SIZE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    return -1;
  for (size_t off = 0; off < MMAP_SIZE; off += 4096)
    p[off] = (unsigned char)(seed + off);
  p[MMAP_SIZE - 1] = (unsigned char)seed;
  return munmap(p, MMAP_SIZE);
}

static int write_output(uint32_t checksum)
{
  const char *path = "/mnt/largeworkload/out.bin";
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
  if (fd < 0)
    return -1;

  for (int i = 0; i < OUTPUT_CHUNKS; i++) {
    for (int j = 0; j < OUTPUT_CHUNK_SIZE; j++)
      io_buf[j] = (unsigned char)(checksum + i + j);
    if (write_all(fd, io_buf, OUTPUT_CHUNK_SIZE) != 0) {
      close(fd);
      return -1;
    }
  }
  close(fd);
  if (chmod(path, 0755) != 0)
    return -1;

  struct stat st;
  if (stat(path, &st) != 0)
    return -1;
  if (st.st_size != OUTPUT_CHUNKS * OUTPUT_CHUNK_SIZE)
    return -1;
  if (access(path, X_OK) != 0)
    return -1;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  ssize_t n = read(fd, io_buf, sizeof(io_buf));
  close(fd);
  return n > 0 ? 0 : -1;
}

static void cleanup_files(void)
{
  char path[64];

  for (int i = 0; i < INPUT_COUNT; i++) {
    input_path(i, path, sizeof(path));
    unlink(path);
  }
  unlink("/mnt/largeworkload/out.bin");
  rmdir("/mnt/largeworkload");

  for (int i = 0; i < HEAP_CHUNKS; i++) {
    free(heap_chunks[i]);
    heap_chunks[i] = NULL;
  }
}

static void workload_child(int progress_fd)
{
  uint32_t checksum = 0;

  progress(progress_fd, 'S');
  if (create_inputs() != 0)
    _exit(10);
  progress(progress_fd, 'C');
  if (read_inputs(&checksum) != 0)
    _exit(11);
  progress(progress_fd, 'R');
  if (grow_heap(checksum) != 0)
    _exit(12);
  progress(progress_fd, 'H');
  if (touch_mapping(checksum) != 0)
    _exit(13);
  progress(progress_fd, 'M');
  if (write_output(checksum) != 0)
    _exit(14);
  progress(progress_fd, 'W');
  cleanup_files();
  progress(progress_fd, 'E');
  _exit(0);
}

static int note_phase(unsigned int *mask, char ch)
{
  const char *phases = "SCRHMWE";
  for (int i = 0; phases[i]; i++) {
    if (phases[i] == ch) {
      *mask |= 1u << i;
      return 1;
    }
  }
  return 0;
}

static void drain_progress(int fd, unsigned int *mask)
{
  for (;;) {
    char ch = 0;
    ssize_t n = read(fd, &ch, 1);
    if (n == 1) {
      note_phase(mask, ch);
      continue;
    }
    if (n < 0 && errno == EAGAIN)
      return;
    return;
  }
}

int main(void)
{
  int pfd[2];
  pid_t child;
  int status = 0;
  unsigned int mask = 0;
  const unsigned int want_mask = (1u << 7) - 1u;

  printf("=== largeworkloadtest: file heap map write reap ===\n");

  cleanup_files();

  if (pipe2(pfd, O_NONBLOCK) != 0) {
    CHECK("progress pipe", 0);
    return 1;
  }

  child = fork();
  if (child == 0) {
    close(pfd[0]);
    workload_child(pfd[1]);
  }

  close(pfd[1]);
  if (child < 0) {
    CHECK("fork workload child", 0);
    close(pfd[0]);
    return 1;
  }

  for (int i = 0; i < 2000; i++) {
    drain_progress(pfd[0], &mask);
    pid_t got = waitpid(child, &status, WNOHANG);
    if (got == child)
      break;
    if (got < 0)
      break;
    usleep(10000);
  }
  drain_progress(pfd[0], &mask);
  close(pfd[0]);

  if (!(WIFEXITED(status) || WIFSIGNALED(status))) {
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
  }

  CHECK("child reached create phase", (mask & (1u << 1)) != 0);
  CHECK("child completed sequential read phase", (mask & (1u << 2)) != 0);
  CHECK("child completed multi-megabyte heap phase", (mask & (1u << 3)) != 0);
  CHECK("child completed private mapping phase", (mask & (1u << 4)) != 0);
  CHECK("child completed small-chunk output phase", (mask & (1u << 5)) != 0);
  CHECK("child reached exit phase", (mask & (1u << 6)) != 0);
  CHECK("parent reaped successful child",
        WIFEXITED(status) && WEXITSTATUS(status) == 0 && mask == want_mask);

  cleanup_files();

  printf("=== largeworkloadtest: %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail > 0 ? 1 : 0;
}
