#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <syscall.h>

struct linux_dirent64 {
    unsigned long  d_ino;
    long           d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

static long getdents64(int fd, void *buf, unsigned long count) {
    return syscall(SYS_getdents64, fd, buf, count);
}

static void ls_dir(const char *path) {
    int fd = open(path, 0);
    if (fd < 0) {
        printf("ls: cannot access '%s'\n", path);
        return;
    }

    char buf[2048];
    long nread;
    while ((nread = getdents64(fd, buf, sizeof(buf))) > 0) {
        long pos = 0;
        while (pos < nread) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + pos);
            if (d->d_name[0] != '.')
                printf("%s\n", d->d_name);
            pos += d->d_reclen;
        }
    }
    close(fd);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        ls_dir(".");
    } else {
        for (int i = 1; i < argc; i++) {
            if (argc > 2)
                printf("%s:\n", argv[i]);
            ls_dir(argv[i]);
        }
    }
    return 0;
}
