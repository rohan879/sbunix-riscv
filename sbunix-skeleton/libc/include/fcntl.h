#ifndef _FCNTL_H
#define _FCNTL_H

#include <sys/types.h>

/*
 * File access modes (bits 0-1 are mutually exclusive).
 * These match the Linux RISC-V ABI values.
 */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_ACCMODE   3

/* File creation / status flags */
#define O_CREAT     0100    /* create file if it does not exist */
#define O_EXCL      0200    /* error if O_CREAT and file already exists */
#define O_NOCTTY    0400    /* do not assign controlling terminal */
#define O_TRUNC     01000   /* truncate to zero length */
#define O_APPEND    02000   /* write: append to end of file */
#define O_NONBLOCK  04000   /* non-blocking I/O */
#define O_DSYNC     010000  /* synchronize data */
#define O_SYNC      04010000 /* synchronize data and metadata */
#define O_DIRECTORY 0200000 /* fail if not a directory */
#define O_NOFOLLOW  0400000 /* do not follow symlinks */
#define O_CLOEXEC   02000000 /* set FD_CLOEXEC */

/* Special value for openat() — use CWD as base */
#define AT_FDCWD    (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define AT_SYMLINK_FOLLOW 0x400
#define AT_EACCESS 0x200

/* fcntl() commands */
#define F_DUPFD     0
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4
#define F_GETLK     5
#define F_SETLK     6
#define F_SETLKW    7

#define F_RDLCK     0
#define F_WRLCK     1
#define F_UNLCK     2

/* File descriptor flags for F_GETFD / F_SETFD */
#define FD_CLOEXEC  1

struct flock {
  short l_type;
  short l_whence;
  off_t l_start;
  off_t l_len;
  pid_t l_pid;
};

int open(const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int fcntl(int fd, int cmd, ...);

#endif /* _FCNTL_H */
