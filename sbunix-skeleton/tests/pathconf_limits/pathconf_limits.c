#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

static int g_pass;
static int g_fail;

#define CHECK(label, cond) do { \
  if (cond) { \
    printf("[PASS] pathconf_limits: %s\n", label); \
    g_pass++; \
  } else { \
    printf("[FAIL] pathconf_limits: %s errno=%d\n", label, errno); \
    g_fail++; \
  } \
} while (0)

static int expect_long(const char *label, long got, long want)
{
  CHECK(label, got == want);
  return got == want;
}

static int expect_errno(long ret, int want)
{
  return ret == -1 && errno == want;
}

int main(void)
{
  int fd;

  printf("=== pathconf_limits ===\n");

  errno = 0;
  expect_long("_PC_FILESIZEBITS", pathconf("/mnt", _PC_FILESIZEBITS),
              FILESIZEBITS);
  errno = 0;
  expect_long("_PC_NAME_MAX", pathconf("/mnt", _PC_NAME_MAX), NAME_MAX);
  errno = 0;
  expect_long("_PC_PATH_MAX", pathconf("/mnt", _PC_PATH_MAX), PATH_MAX);
  errno = 0;
  expect_long("_PC_PIPE_BUF", pathconf("/mnt", _PC_PIPE_BUF), PIPE_BUF);
  errno = 0;
  expect_long("_PC_LINK_MAX", pathconf("/mnt", _PC_LINK_MAX), LINK_MAX);
  errno = 0;
  expect_long("_PC_SYMLINK_MAX", pathconf("/mnt", _PC_SYMLINK_MAX),
              SYMLINK_MAX);
  errno = 0;
  expect_long("_PC_REC_XFER_ALIGN", pathconf("/mnt", _PC_REC_XFER_ALIGN),
              POSIX_REC_XFER_ALIGN);
  errno = 0;
  expect_long("_PC_CHOWN_RESTRICTED", pathconf("/mnt", _PC_CHOWN_RESTRICTED),
              _POSIX_CHOWN_RESTRICTED);
  errno = 0;
  expect_long("_PC_NO_TRUNC", pathconf("/mnt", _PC_NO_TRUNC),
              _POSIX_NO_TRUNC);
  errno = 0;
  expect_long("_PC_VDISABLE", pathconf("/mnt", _PC_VDISABLE),
              _POSIX_VDISABLE);

  fd = open("/mnt", O_RDONLY | O_DIRECTORY);
  CHECK("open /mnt dirfd", fd >= 0);
  if (fd >= 0) {
    errno = 0;
    expect_long("fpathconf NAME_MAX", fpathconf(fd, _PC_NAME_MAX), NAME_MAX);
    errno = 0;
    expect_long("fpathconf PATH_MAX", fpathconf(fd, _PC_PATH_MAX), PATH_MAX);
    close(fd);
  }

  errno = 0;
  CHECK("valid unsupported name leaves errno unchanged",
        pathconf("/mnt", _PC_ASYNC_IO) == -1 && errno == 0);
  errno = 0;
  CHECK("invalid name reports EINVAL",
        expect_errno(pathconf("/mnt", 9999), EINVAL));
  errno = 0;
  CHECK("missing path reports ENOENT",
        expect_errno(pathconf("/mnt/pathconf_limits_missing", _PC_NAME_MAX),
                     ENOENT));
  errno = 0;
  CHECK("bad fd reports EBADF",
        expect_errno(fpathconf(-1, _PC_NAME_MAX), EBADF));

  printf("pathconf_limits: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
