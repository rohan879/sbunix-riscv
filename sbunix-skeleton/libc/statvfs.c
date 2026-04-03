#include <sys/statvfs.h>
#include <syscall.h>
#include <errno.h>

struct linux_statfs_k {
  unsigned long f_type;
  unsigned long f_bsize;
  unsigned long f_blocks;
  unsigned long f_bfree;
  unsigned long f_bavail;
  unsigned long f_files;
  unsigned long f_ffree;
  unsigned long f_fsid[2];
  unsigned long f_namelen;
  unsigned long f_frsize;
  unsigned long f_flags;
  unsigned long f_spare[4];
};

static int statvfs_from_kstatfs(struct statvfs *dst, const struct linux_statfs_k *src) {
  if (!dst || !src) {
    errno = EINVAL;
    return -1;
  }
  dst->f_bsize = src->f_bsize;
  dst->f_frsize = src->f_frsize ? src->f_frsize : src->f_bsize;
  dst->f_blocks = src->f_blocks;
  dst->f_bfree = src->f_bfree;
  dst->f_bavail = src->f_bavail;
  dst->f_files = src->f_files;
  dst->f_ffree = src->f_ffree;
  dst->f_favail = src->f_ffree;
  dst->f_fsid = src->f_fsid[0] ^ src->f_fsid[1];
  dst->f_flag = src->f_flags;
  dst->f_namemax = src->f_namelen;
  return 0;
}

int statvfs(const char *path, struct statvfs *buf) {
  struct linux_statfs_k st;
  if (__syscall_ret(syscall(SYS_statfs, path, &st)) < 0)
    return -1;
  return statvfs_from_kstatfs(buf, &st);
}

int fstatvfs(int fd, struct statvfs *buf) {
  struct linux_statfs_k st;
  if (__syscall_ret(syscall(SYS_fstatfs, fd, &st)) < 0)
    return -1;
  return statvfs_from_kstatfs(buf, &st);
}
