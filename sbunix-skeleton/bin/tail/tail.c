/*
 * tail.c -- print last N lines (or bytes) of files or stdin.
 *
 * Flags: -n NUM (lines), -c NUM (bytes).  Default: 10 lines.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define TAIL_MAX_INPUT (1024 * 1024)  /* 1 MiB max buffered input */

static char gbuf[TAIL_MAX_INPUT];

static int tail_bytes(int fd, long n) {
    ssize_t total = 0, r;
    while (total < TAIL_MAX_INPUT && (r = read(fd, gbuf + total, TAIL_MAX_INPUT - total)) > 0)
        total += r;
    long start = total > n ? total - n : 0;
    if (total - start > 0)
        write(STDOUT_FILENO, gbuf + start, (size_t)(total - start));
    return 0;
}

static int tail_lines(int fd, long n) {
    ssize_t total = 0, r;
    while (total < TAIL_MAX_INPUT && (r = read(fd, gbuf + total, TAIL_MAX_INPUT - total)) > 0)
        total += r;

    /* Count newlines from end */
    long count = 0;
    ssize_t pos = total;
    while (pos > 0) {
        pos--;
        if (gbuf[pos] == '\n') {
            count++;
            if (count > n) {
                pos++;  /* skip past this newline */
                break;
            }
        }
    }
    if (total - pos > 0)
        write(STDOUT_FILENO, gbuf + pos, (size_t)(total - pos));
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
            dprintf(2, "usage: tail [-n NUM] [-c NUM] [FILE...]\n");
            return 1;
        }
    }

    int nfiles = argc - optind;
    if (nfiles == 0) {
        return byte_mode ? tail_bytes(STDIN_FILENO, nlines)
                         : tail_lines(STDIN_FILENO, nlines);
    }

    for (int i = optind; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(2, "tail: %s: %s\n", argv[i], strerror(errno));
            continue;
        }
        if (nfiles > 1)
            printf("==> %s <==\n", argv[i]);
        if (byte_mode)
            tail_bytes(fd, nlines);
        else
            tail_lines(fd, nlines);
        close(fd);
        if (nfiles > 1 && i < argc - 1)
            printf("\n");
    }
    return 0;
}
