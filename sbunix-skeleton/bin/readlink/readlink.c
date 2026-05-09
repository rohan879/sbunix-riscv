/*
 * readlink.c -- display the contents of a symbolic link.
 *
 * Usage: readlink [-n] file
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    int no_newline = 0;
    int opt;

    while ((opt = getopt(argc, argv, "n")) != -1) {
        switch (opt) {
        case 'n':
            no_newline = 1;
            break;
        default:
            dprintf(2, "usage: readlink [-n] file\n");
            return 1;
        }
    }

    if (argc - optind != 1) {
        dprintf(2, "usage: readlink [-n] file\n");
        return 1;
    }

    char buf[256];
    ssize_t n = readlink(argv[optind], buf, sizeof(buf));
    if (n < 0) {
        dprintf(2, "readlink: %s: %s\n", argv[optind], strerror(errno));
        return 1;
    }

    ssize_t off = 0;
    while (off < n) {
        ssize_t wr = write(1, buf + off, (size_t)(n - off));
        if (wr <= 0) {
            dprintf(2, "readlink: write: %s\n", strerror(errno));
            return 1;
        }
        off += wr;
    }
    if (!no_newline && write(1, "\n", 1) != 1) {
        dprintf(2, "readlink: write: %s\n", strerror(errno));
        return 1;
    }

    return 0;
}
