#ifndef _SYS_STATVFS_H
#define _SYS_STATVFS_H

#include <sys/types.h>

struct statvfs {
    unsigned long f_bsize;    /* filesystem block size */
    unsigned long f_frsize;   /* fragment size */
    unsigned long f_blocks;   /* total blocks (in f_frsize units) */
    unsigned long f_bfree;    /* free blocks */
    unsigned long f_bavail;   /* free blocks for unprivileged users */
    unsigned long f_files;    /* total inodes */
    unsigned long f_ffree;    /* free inodes */
    unsigned long f_favail;   /* free inodes for unprivileged users */
    unsigned long f_fsid;     /* filesystem ID */
    unsigned long f_flag;     /* mount flags */
    unsigned long f_namemax;  /* maximum filename length */
};

/* Mount flags for f_flag */
#define ST_RDONLY    1   /* read-only filesystem */
#define ST_NOSUID    2   /* no set-uid/set-gid allowed */

int statvfs(const char *path, struct statvfs *buf);
int fstatvfs(int fd, struct statvfs *buf);

#endif /* _SYS_STATVFS_H */
