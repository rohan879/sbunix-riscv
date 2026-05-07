#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_pass;
static int g_fail;

static void check(const char *name, int ok) {
  if (ok) {
    g_pass++;
    printf("[PASS] %s\n", name);
  } else {
    g_fail++;
    printf("[FAIL] %s errno=%d\n", name, errno);
  }
}

static int read_file(const char *path, char *buf, int bufsz) {
  int fd = open(path, O_RDONLY);
  int total = 0;

  if (fd < 0)
    return -1;
  while (total < bufsz - 1) {
    int n = read(fd, buf + total, bufsz - 1 - total);
    if (n < 0) {
      close(fd);
      return -1;
    }
    if (n == 0)
      break;
    total += n;
  }
  close(fd);
  buf[total] = '\0';
  return total;
}

static void expect_contains(const char *label, const char *path,
                            const char *needle) {
  char buf[768];
  struct stat st;
  int n = read_file(path, buf, sizeof(buf));

  check(label, n > 0 && strstr(buf, needle) != 0);
  check("procfs_diagnostics_edges: stat regular",
        stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

int main(void) {
  printf("=== procfs_diagnostics_edges ===\n");

  expect_contains("procfs_diagnostics_edges: kmsg reads",
                  "/proc/kmsg", "SBUnix");
  expect_contains("procfs_diagnostics_edges: stat has cpu",
                  "/proc/stat", "cpu  ");
  expect_contains("procfs_diagnostics_edges: stat has procs",
                  "/proc/stat", "procs_running");
  expect_contains("procfs_diagnostics_edges: interrupts has cpu header",
                  "/proc/interrupts", "CPU0");
  expect_contains("procfs_diagnostics_edges: interrupts has timer",
                  "/proc/interrupts", "RISC-V timer");
  expect_contains("procfs_diagnostics_edges: slabinfo version",
                  "/proc/slabinfo", "slabinfo - version:");
  expect_contains("procfs_diagnostics_edges: buddyinfo node",
                  "/proc/buddyinfo", "Node 0, zone");

  printf("=== procfs_diagnostics_edges: %d passed, %d failed ===\n",
         g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
