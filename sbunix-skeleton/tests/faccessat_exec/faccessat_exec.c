#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_pass;
static int g_fail;

#define CHECK(label, cond) do { \
  if (cond) { \
    printf("[PASS] faccessat_exec: %s\n", label); \
    g_pass++; \
  } else { \
    printf("[FAIL] faccessat_exec: %s errno=%d\n", label, errno); \
    g_fail++; \
  } \
} while (0)

static int write_file(const char *path, mode_t mode)
{
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
  const char *data = "payload\n";
  int ok;

  if (fd < 0)
    return -1;
  ok = write(fd, data, strlen(data)) == (ssize_t)strlen(data);
  close(fd);
  return ok ? 0 : -1;
}

static int expect_errno(int ret, int want)
{
  return ret == -1 && errno == want;
}

int main(void)
{
  const char *base = "/mnt/faccessat_exec";
  const char *path = "/mnt/faccessat_exec/tool";
  int dfd;

  printf("=== faccessat_exec ===\n");

  unlink(path);
  rmdir(base);
  CHECK("mkdir base", mkdir(base, 0755) == 0);
  CHECK("create non-executable file", write_file(path, 0644) == 0);

  errno = 0;
  CHECK("access F_OK succeeds", access(path, F_OK) == 0);
  errno = 0;
  CHECK("access R_OK succeeds", access(path, R_OK) == 0);
  errno = 0;
  CHECK("access W_OK succeeds", access(path, W_OK) == 0);

  errno = 0;
  CHECK("access X_OK rejects missing execute bit",
        expect_errno(access(path, X_OK), EACCES));
  errno = 0;
  CHECK("faccessat X_OK rejects missing execute bit",
        expect_errno(faccessat(AT_FDCWD, path, X_OK, 0), EACCES));

  CHECK("chmod adds execute bits", chmod(path, 0755) == 0);
  errno = 0;
  CHECK("access X_OK succeeds after chmod", access(path, X_OK) == 0);
  errno = 0;
  CHECK("faccessat X_OK succeeds after chmod",
        faccessat(AT_FDCWD, path, X_OK, 0) == 0);

  dfd = open(base, O_RDONLY | O_DIRECTORY);
  CHECK("open dirfd", dfd >= 0);
  if (dfd >= 0) {
    int fd;

    CHECK("chmod removes execute bits", chmod(path, 0644) == 0);
    errno = 0;
    CHECK("dirfd-relative X_OK rejects missing execute bit",
          expect_errno(faccessat(dfd, "tool", X_OK, 0), EACCES));
    CHECK("chmod owner execute only", chmod(path, 0100) == 0);
    errno = 0;
    CHECK("dirfd-relative X_OK accepts owner execute",
          faccessat(dfd, "tool", X_OK, 0) == 0);

    fd = open(path, O_RDONLY);
    CHECK("open file fd", fd >= 0);
    if (fd >= 0) {
      errno = 0;
      CHECK("AT_EMPTY_PATH X_OK checks fd",
            faccessat(fd, "", X_OK, AT_EMPTY_PATH) == 0);
      close(fd);
    }
    close(dfd);
  }

  errno = 0;
  CHECK("invalid mode rejected",
        expect_errno(faccessat(AT_FDCWD, path, 8, 0), EINVAL));
  errno = 0;
  CHECK("invalid flag rejected",
        expect_errno(faccessat(AT_FDCWD, path, F_OK, 0x8000), EINVAL));
  errno = 0;
  CHECK("missing path reports ENOENT",
        expect_errno(faccessat(AT_FDCWD, "/mnt/faccessat_exec/missing", F_OK, 0),
                     ENOENT));

  unlink(path);
  rmdir(base);

  printf("faccessat_exec: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
