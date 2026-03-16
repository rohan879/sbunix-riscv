/*
 * head.c -- print first N lines (or bytes) of files or stdin.
 *
 * Flags: -n NUM (lines), -c NUM (bytes).  Default: 10 lines.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int head_lines(int fd, long n) {
    char buf[4096];
    long count = 0;
    ssize_t r;
    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < r; i++) {
            write(STDOUT_FILENO, &buf[i], 1);
            if (buf[i] == '\n' && ++count >= n)
                return 0;
        }
    }
    return 0;
}

static int head_bytes(int fd, long n) {
    char buf[4096];
    long left = n;
    while (left > 0) {
        size_t want = (size_t)left < sizeof(buf) ? (size_t)left : sizeof(buf);
        ssize_t r = read(fd, buf, want);
        if (r <= 0) break;
        write(STDOUT_FILENO, buf, (size_t)r);
        left -= r;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    long nlines = 10;
    int byte_mode = 0;
    int opt;

    while ((opt = getopt(argc, argv, "n:c:")) != -1) {
        switch (opt) {
        case 'n': nlines = atol(optarg); byte_mode = 0; break;
        case 'c': nlines = atol(optarg); byte_mode = 1; break;
        default:
            dprintf(2, "usage: head [-n NUM] [-c NUM] [FILE...]\n");
            return 1;
        }
    }

    int nfiles = argc - optind;
    if (nfiles == 0) {
        return byte_mode ? head_bytes(STDIN_FILENO, nlines)
                         : head_lines(STDIN_FILENO, nlines);
    }

    for (int i = optind; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(2, "head: %s: %s\n", argv[i], strerror(errno));
            continue;
        }
        if (nfiles > 1)
            printf("==> %s <==\n", argv[i]);
        if (byte_mode)
            head_bytes(fd, nlines);
        else
            head_lines(fd, nlines);
        close(fd);
        if (nfiles > 1 && i < argc - 1)
            printf("\n");
    }
    return 0;
}
