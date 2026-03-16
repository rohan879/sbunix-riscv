/*
 * wc.c -- count lines, words, and bytes.
 *
 * Flags: -l (lines), -w (words), -c (bytes).  Default: all three.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int opt_lines, opt_words, opt_bytes;

static void wc_fd(int fd, const char *name, long *tl, long *tw, long *tb) {
    char buf[4096];
    long lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        bytes += n;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n')
                lines++;
            if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' ||
                buf[i] == '\r' || buf[i] == '\v' || buf[i] == '\f') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }

    if (opt_lines) printf("%7ld", lines);
    if (opt_words) printf("%7ld", words);
    if (opt_bytes) printf("%7ld", bytes);
    if (name)
        printf(" %s", name);
    printf("\n");

    *tl += lines;
    *tw += words;
    *tb += bytes;
}

int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "lwc")) != -1) {
        switch (opt) {
        case 'l': opt_lines = 1; break;
        case 'w': opt_words = 1; break;
        case 'c': opt_bytes = 1; break;
        default:
            dprintf(2, "usage: wc [-lwc] [FILE...]\n");
            return 1;
        }
    }
    /* Default: show all */
    if (!opt_lines && !opt_words && !opt_bytes) {
        opt_lines = opt_words = opt_bytes = 1;
    }

    long tl = 0, tw = 0, tb = 0;
    int nfiles = argc - optind;

    if (nfiles == 0) {
        wc_fd(STDIN_FILENO, NULL, &tl, &tw, &tb);
        return 0;
    }

    for (int i = optind; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(2, "wc: %s: %s\n", argv[i], strerror(errno));
            continue;
        }
        wc_fd(fd, argv[i], &tl, &tw, &tb);
        close(fd);
    }

    if (nfiles > 1) {
        if (opt_lines) printf("%7ld", tl);
        if (opt_words) printf("%7ld", tw);
        if (opt_bytes) printf("%7ld", tb);
        printf(" total\n");
    }
    return 0;
}
