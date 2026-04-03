#ifndef _DIRENT_H
#define _DIRENT_H

#include <sys/types.h>

typedef unsigned short reclen_t;

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
#define DT_MQ      15
#define DT_SEM     16
#define DT_SHM     17
#define DT_TMO     18

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
    reclen_t       d_reclen;
    reclen_t       d_namlen;
    char           d_name[NAME_MAX + 1]; /* null-terminated filename */
};

struct posix_dent {
    ino_t          d_ino;
    reclen_t       d_reclen;
    unsigned char  d_type;
    char           d_name[];
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
int            dirfd(DIR *dirp);
DIR           *fdopendir(int fd);
void           seekdir(DIR *dirp, long loc);
long           telldir(DIR *dirp);
int            readdir_r(DIR *dirp, struct dirent *entry,
                         struct dirent **result);
int            alphasort(const struct dirent **a, const struct dirent **b);
int            scandir(const char *dirp, struct dirent ***namelist,
                       int (*filter)(const struct dirent *),
                       int (*compar)(const struct dirent **,
                                     const struct dirent **));
ssize_t        posix_getdents(int fd, void *buf, size_t nbytes, int flags);

/* Low-level syscall wrapper */
long getdents64(int fd, struct linux_dirent64 *buf, unsigned int count);

#endif /* _DIRENT_H */
