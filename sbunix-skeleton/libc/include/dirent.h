#ifndef _DIRENT_H
#define _DIRENT_H

#include <sys/types.h>

/* d_type values returned by getdents64 */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14

/*
 * linux_dirent64 — the structure filled by SYS_getdents64.
 * This matches the Linux kernel ABI exactly.
 */
struct linux_dirent64 {
    unsigned long  d_ino;     /* inode number */
    long           d_off;     /* offset to next entry */
    unsigned short d_reclen;  /* length of this record */
    unsigned char  d_type;    /* file type (DT_* above) */
    char           d_name[];  /* null-terminated filename */
};

/*
 * POSIX struct dirent — a portable, higher-level view.
 * We store the raw d_ino and d_type, and cap d_name at NAME_MAX+1.
 */
#define NAME_MAX 255

struct dirent {
    unsigned long  d_ino;           /* inode number */
    unsigned char  d_type;          /* file type */
    char           d_name[NAME_MAX + 1]; /* null-terminated filename */
};

/* Opaque directory stream handle */
typedef struct {
    int   fd;       /* underlying file descriptor */
    char  buf[2048]; /* kernel dirent64 read buffer */
    int   buf_pos;  /* current position in buf */
    int   buf_len;  /* valid bytes in buf */
} DIR;

/* POSIX directory functions */
DIR           *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int            closedir(DIR *dirp);
void           rewinddir(DIR *dirp);

/* Low-level syscall wrapper */
long getdents64(int fd, struct linux_dirent64 *buf, unsigned int count);

#endif /* _DIRENT_H */
