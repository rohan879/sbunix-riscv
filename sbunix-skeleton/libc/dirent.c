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
#include <errno.h>
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
    dp->fd       = fd;
    dp->buf_pos  = 0;
    dp->buf_len  = 0;
    dp->tell_off = 0; /* BF: init d_off cookie for telldir */
    return dp;
}

DIR *fdopendir(int fd) {
    if (fd < 0) {
        errno = EBADF;
        return (void *)0;
    }
    DIR *dp = (DIR *)malloc(sizeof(DIR));
    if (!dp)
        return (void *)0;
    dp->fd       = fd;
    dp->buf_pos  = 0;
    dp->buf_len  = 0;
    dp->tell_off = 0; /* BF: init d_off cookie for telldir */
    return dp;
}

int dirfd(DIR *dirp) {
    if (!dirp) {
        errno = EINVAL;
        return -1;
    }
    return dirp->fd;
}

ssize_t posix_getdents(int fd, void *buf, size_t nbytes, int flags) {
    (void)flags;
    if (!buf || nbytes == 0) {
        errno = EINVAL;
        return -1;
    }

    char kbuf[2048];
    long kn = getdents64(fd, (struct linux_dirent64 *)kbuf, sizeof(kbuf));
    if (kn <= 0)
        return (ssize_t)kn;

    size_t out = 0;
    size_t off = 0;
    while (off < (size_t)kn) {
        struct linux_dirent64 *kd = (struct linux_dirent64 *)(kbuf + off);
        size_t nlen = strnlen(kd->d_name, NAME_MAX);
        size_t reclen = sizeof(struct posix_dent) + nlen + 1;
        if (out + reclen > nbytes) {
            if (out == 0) {
                errno = EINVAL;
                return -1;
            }
            break;
        }

        struct posix_dent *pd = (struct posix_dent *)((char *)buf + out);
        pd->d_ino = (ino_t)kd->d_ino;
        pd->d_reclen = (reclen_t)reclen;
        pd->d_type = kd->d_type;
        memcpy(pd->d_name, kd->d_name, nlen);
        pd->d_name[nlen] = '\0';

        out += reclen;
        off += kd->d_reclen;
    }

    return (ssize_t)out;
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

        /* BF: Save d_off cookie for telldir/seekdir.  The kernel's d_off
         * is an opaque position cookie, not a byte offset — using it with
         * lseek(SEEK_SET) resumes reading from this entry's successor. */
        dirp->tell_off = kd->d_off;

        /* Copy into the POSIX dirent */
        ent.d_ino  = kd->d_ino;
        ent.d_type = kd->d_type;
        ent.d_reclen = (reclen_t)(sizeof(struct dirent));
        ent.d_namlen = (reclen_t)strnlen(kd->d_name, NAME_MAX);
        strncpy(ent.d_name, kd->d_name, NAME_MAX);
        ent.d_name[NAME_MAX] = '\0';

        return &ent;
    }
}

int readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result) {
    if (!dirp || !entry || !result)
        return EINVAL;
    struct dirent *de = readdir(dirp);
    if (!de) {
        *result = (void *)0;
        return 0;
    }
    *entry = *de;
    *result = entry;
    return 0;
}

long telldir(DIR *dirp) {
    if (!dirp) {
        errno = EINVAL;
        return -1;
    }
    /* BF: Return the d_off cookie from the last readdir entry.
     * The old code used lseek(SEEK_CUR) minus unread buffer bytes,
     * which is incorrect — d_off is an opaque kernel cookie, not a
     * byte offset into a linear stream. */
    return dirp->tell_off;
}

void seekdir(DIR *dirp, long loc) {
    if (!dirp)
        return;
    if (loc < 0)
        loc = 0;
    if (lseek(dirp->fd, (off_t)loc, SEEK_SET) < 0)
        return;
    dirp->buf_pos = 0;
    dirp->buf_len = 0;
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
    dirp->buf_pos  = 0;
    dirp->buf_len  = 0;
    dirp->tell_off = 0; /* BF: reset d_off cookie on rewind */
}

int alphasort(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

int scandir(const char *path, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
    if (!path || !namelist) {
        errno = EINVAL;
        return -1;
    }

    DIR *dp = opendir(path);
    if (!dp)
        return -1;

    size_t cap = 16;
    size_t count = 0;
    struct dirent **list = (struct dirent **)malloc(cap * sizeof(*list));
    if (!list) {
        closedir(dp);
        return -1;
    }

    struct dirent *de;
    while ((de = readdir(dp)) != (void *)0) {
        if (filter && !filter(de))
            continue;
        if (count == cap) {
            size_t newcap = cap * 2;
            struct dirent **tmp = (struct dirent **)realloc(list, newcap * sizeof(*tmp));
            if (!tmp)
                break;
            list = tmp;
            cap = newcap;
        }
        struct dirent *copy = (struct dirent *)malloc(sizeof(struct dirent));
        if (!copy)
            break;
        *copy = *de;
        list[count++] = copy;
    }
    closedir(dp);

    if (compar && count > 1)
        qsort(list, count, sizeof(*list), (int (*)(const void *, const void *))compar);

    *namelist = list;
    return (int)count;
}
