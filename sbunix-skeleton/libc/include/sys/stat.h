#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <sys/types.h>
#include <time.h>

/*
 * struct stat — matches the Linux RISC-V (riscv64) kernel ABI layout.
 */
struct stat {
    dev_t     st_dev;       /* device ID of containing filesystem */
    ino_t     st_ino;       /* inode number */
    mode_t    st_mode;      /* file type and permissions */
    nlink_t   st_nlink;     /* number of hard links */
    uid_t     st_uid;       /* user ID of owner */
    gid_t     st_gid;       /* group ID of owner */
    dev_t     st_rdev;      /* device ID (if special file) */
    unsigned long __pad1;
    off_t     st_size;      /* total size, in bytes */
    blksize_t st_blksize;   /* block size for filesystem I/O */
    int       __pad2;
    blkcnt_t  st_blocks;    /* number of 512-byte blocks allocated */
    struct timespec st_atim; /* time of last access */
    struct timespec st_mtim; /* time of last modification */
    struct timespec st_ctim; /* time of last status change */
    unsigned int __unused[2];
};

/* Convenience aliases */
#define st_atime  st_atim.tv_sec
#define st_mtime  st_mtim.tv_sec
#define st_ctime  st_ctim.tv_sec

/* File type bits in st_mode */
#define S_IFMT   0170000  /* mask for file type */
#define S_IFSOCK 0140000  /* socket */
#define S_IFLNK  0120000  /* symbolic link */
#define S_IFREG  0100000  /* regular file */
#define S_IFBLK  0060000  /* block device */
#define S_IFDIR  0040000  /* directory */
#define S_IFCHR  0020000  /* character device */
#define S_IFIFO  0010000  /* FIFO / named pipe */

/* File type test macros */
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* Permission bits */
#define S_ISUID  04000  /* set-UID bit */
#define S_ISGID  02000  /* set-GID bit */
#define S_ISVTX  01000  /* sticky bit */

#define S_IRWXU  00700  /* owner: rwx */
#define S_IRUSR  00400  /* owner: read */
#define S_IWUSR  00200  /* owner: write */
#define S_IXUSR  00100  /* owner: execute */

#define S_IRWXG  00070  /* group: rwx */
#define S_IRGRP  00040  /* group: read */
#define S_IWGRP  00020  /* group: write */
#define S_IXGRP  00010  /* group: execute */

#define S_IRWXO  00007  /* others: rwx */
#define S_IROTH  00004  /* others: read */
#define S_IWOTH  00002  /* others: write */
#define S_IXOTH  00001  /* others: execute */

/* Common permission combos */
#define S_IRUGO  (S_IRUSR | S_IRGRP | S_IROTH)
#define S_IWUGO  (S_IWUSR | S_IWGRP | S_IWOTH)
#define S_IXUGO  (S_IXUSR | S_IXGRP | S_IXOTH)

int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int lstat(const char *path, struct stat *buf);
int mkdir(const char *path, mode_t mode);
mode_t umask(mode_t mask);

#endif /* _SYS_STAT_H */
