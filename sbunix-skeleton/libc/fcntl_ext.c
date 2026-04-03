#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int posix_fadvise(int fd, off_t offset, off_t len, int advice) {
  (void)fd;
  (void)offset;
  (void)len;
  (void)advice;
  return 0;
}

int posix_fallocate(int fd, off_t offset, off_t len) {
  if (offset < 0 || len < 0) {
    errno = EINVAL;
    return EINVAL;
  }
  off_t end = offset + len;
  off_t cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0)
    return errno;
  off_t sz = lseek(fd, 0, SEEK_END);
  if (sz < 0)
    return errno;
  if (end > sz) {
    if (ftruncate(fd, end) < 0)
      return errno;
  }
  (void)lseek(fd, cur, SEEK_SET);
  return 0;
}
