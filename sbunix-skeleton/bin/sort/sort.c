/*
 * sort.c -- sort lines of text.
 *
 * Flags: -r (reverse), -n (numeric), -u (unique).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int opt_reverse, opt_numeric, opt_unique;

#define MAX_LINES 16384

static char *lines[MAX_LINES];
static int nlines;

/* Read all input lines from fd into lines[] array */
static void read_lines(int fd) {
    char buf[4096];
    char line[4096];
    int lpos = 0;
    ssize_t r;

    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < r && nlines < MAX_LINES; i++) {
            if (buf[i] == '\n') {
                line[lpos] = '\0';
                lines[nlines++] = strdup(line);
                lpos = 0;
            } else if (lpos < (int)sizeof(line) - 1) {
                line[lpos++] = buf[i];
            }
        }
    }
    /* Handle last line without trailing newline */
    if (lpos > 0 && nlines < MAX_LINES) {
        line[lpos] = '\0';
        lines[nlines++] = strdup(line);
    }
}

static int cmp_string(const void *a, const void *b) {
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    int r;
    if (opt_numeric) {
        long la = atol(sa);
        long lb = atol(sb);
        r = (la > lb) - (la < lb);
    } else {
        r = strcmp(sa, sb);
    }
    return opt_reverse ? -r : r;
}

int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "rnu")) != -1) {
        switch (opt) {
        case 'r': opt_reverse = 1; break;
        case 'n': opt_numeric = 1; break;
        case 'u': opt_unique = 1; break;
        default:
            dprintf(2, "usage: sort [-rnu] [FILE...]\n");
            return 1;
        }
    }

    if (optind >= argc) {
        read_lines(STDIN_FILENO);
    } else {
        for (int i = optind; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                dprintf(2, "sort: %s: %s\n", argv[i], strerror(errno));
                continue;
            }
            read_lines(fd);
            close(fd);
        }
    }

    qsort(lines, (size_t)nlines, sizeof(char *), cmp_string);

    for (int i = 0; i < nlines; i++) {
        if (opt_unique && i > 0 && strcmp(lines[i], lines[i - 1]) == 0)
            continue;
        printf("%s\n", lines[i]);
    }

    for (int i = 0; i < nlines; i++)
        free(lines[i]);
    return 0;
}
