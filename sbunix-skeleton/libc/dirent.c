/*
 * dirent.c — directory access functions for SBUnix libc.
 *
 * Provides:
 *   getdents64()  — raw syscall wrapper
 *   opendir()     — open a directory, returning a DIR handle
 *   readdir()     — read successive entries from an open DIR
 *   closedir()    — close a DIR handle
 *   rewinddir()   — rewind to the start of a DIR
 *
 * The implementation reads the kernel's linux_dirent64 structures via
 * SYS_getdents64 into an internal buffer and wraps them in the higher-level
 * POSIX struct dirent that callers see.
 */

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syscall.h>
#include <fcntl.h>

/* getdents64 — low-level wrapper */
long getdents64(int fd, struct linux_dirent64 *buf, unsigned int count) {
    return syscall(SYS_getdents64, fd, buf, count);
}

/* opendir — open a directory by name */
DIR *opendir(const char *name) {
    int fd = open(name, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return (void *)0;

    DIR *dp = (DIR *)malloc(sizeof(DIR));
    if (!dp) {
        close(fd);
        return (void *)0;
    }
    dp->fd      = fd;
    dp->buf_pos = 0;
    dp->buf_len = 0;
    return dp;
}

/* readdir — return the next directory entry, or NULL at end / error */
struct dirent *readdir(DIR *dirp) {
    static struct dirent ent;

    if (!dirp)
        return (void *)0;

    while (1) {
        /* If the buffer is exhausted, refill it */
        if (dirp->buf_pos >= dirp->buf_len) {
            long n = getdents64(dirp->fd,
                                (struct linux_dirent64 *)dirp->buf,
                                sizeof(dirp->buf));
            if (n <= 0)
                return (void *)0;  /* EOF or error */
            dirp->buf_pos = 0;
            dirp->buf_len = (int)n;
        }

        struct linux_dirent64 *kd =
            (struct linux_dirent64 *)(dirp->buf + dirp->buf_pos);
        dirp->buf_pos += kd->d_reclen;

        /* Copy into the POSIX dirent */
        ent.d_ino  = kd->d_ino;
        ent.d_type = kd->d_type;
        strncpy(ent.d_name, kd->d_name, NAME_MAX);
        ent.d_name[NAME_MAX] = '\0';

        return &ent;
    }
}

/* closedir — release a DIR handle */
int closedir(DIR *dirp) {
    if (!dirp)
        return -1;
    int fd = dirp->fd;
    free(dirp);
    return close(fd);
}

/* rewinddir — seek back to the start of the directory */
void rewinddir(DIR *dirp) {
    if (!dirp)
        return;
    lseek(dirp->fd, 0, SEEK_SET);
    dirp->buf_pos = 0;
    dirp->buf_len = 0;
}
