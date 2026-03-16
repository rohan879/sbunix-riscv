/*
 * cut.c -- cut fields or character ranges from lines.
 *
 * Flags: -d DELIM, -f FIELDS, -c CHARS.
 * Field/char list: comma-separated, supports N, N-M, N-, -M.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static char delim = '\t';
static int field_mode;   /* 1 = -f, 0 = -c */
static int selected[1024]; /* 1-based positions that are selected */
static int max_sel;

/* Parse a field/char list like "1,3-5,7-" into selected[] */
static void parse_list(const char *s) {
    memset(selected, 0, sizeof(selected));
    max_sel = 0;

    while (*s) {
        int lo = 0, hi = 0;
        if (*s == '-') {
            lo = 1;
            s++;
            hi = atoi(s);
            while (*s >= '0' && *s <= '9') s++;
            if (hi == 0) hi = 1023;
        } else {
            lo = atoi(s);
            while (*s >= '0' && *s <= '9') s++;
            if (*s == '-') {
                s++;
                if (*s >= '0' && *s <= '9') {
                    hi = atoi(s);
                    while (*s >= '0' && *s <= '9') s++;
                } else {
                    hi = 1023;
                }
            } else {
                hi = lo;
            }
        }
        if (lo < 1) lo = 1;
        if (hi > 1023) hi = 1023;
        for (int i = lo; i <= hi; i++) {
            selected[i] = 1;
            if (i > max_sel) max_sel = i;
        }
        if (*s == ',') s++;
    }
}

static void cut_line(char *line) {
    /* Remove trailing newline */
    int len = strlen(line);
    if (len > 0 && line[len - 1] == '\n')
        line[--len] = '\0';

    if (!field_mode) {
        /* Character mode */
        int first = 1;
        for (int i = 0; i < len && i + 1 <= max_sel; i++) {
            if (selected[i + 1]) {
                if (!first) { /* nothing between chars */ }
                putchar(line[i]);
                first = 0;
            }
        }
        putchar('\n');
        return;
    }

    /* Field mode: split by delimiter */
    char *fields[256];
    int nf = 0;
    char *p = line;
    fields[nf++] = p;
    while (*p && nf < 256) {
        if (*p == delim) {
            *p = '\0';
            fields[nf++] = p + 1;
        }
        p++;
    }

    /* If line has no delimiters, print it whole */
    if (nf == 1) {
        printf("%s\n", line);
        return;
    }

    int first = 1;
    for (int i = 0; i < nf; i++) {
        if (i + 1 <= max_sel && selected[i + 1]) {
            if (!first) putchar(delim);
            printf("%s", fields[i]);
            first = 0;
        }
    }
    putchar('\n');
}

static void cut_fd(int fd) {
    char buf[4096];
    char line[4096];
    int lpos = 0;
    ssize_t r;

    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n') {
                line[lpos] = '\0';
                cut_line(line);
                lpos = 0;
            } else if (lpos < (int)sizeof(line) - 2) {
                line[lpos++] = buf[i];
            }
        }
    }
    if (lpos > 0) {
        line[lpos] = '\0';
        cut_line(line);
    }
}

int main(int argc, char *argv[]) {
    int opt;
    const char *list = NULL;

    while ((opt = getopt(argc, argv, "d:f:c:")) != -1) {
        switch (opt) {
        case 'd':
            delim = optarg[0];
            break;
        case 'f':
            field_mode = 1;
            list = optarg;
            break;
        case 'c':
            field_mode = 0;
            list = optarg;
            break;
        default:
            dprintf(2, "usage: cut [-d DELIM] -f FIELDS | -c CHARS [FILE...]\n");
            return 1;
        }
    }

    if (!list) {
        dprintf(2, "cut: must specify -f or -c\n");
        return 1;
    }
    parse_list(list);

    if (optind >= argc) {
        cut_fd(STDIN_FILENO);
    } else {
        for (int i = optind; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                dprintf(2, "cut: %s: %s\n", argv[i], strerror(errno));
                continue;
            }
            cut_fd(fd);
            close(fd);
        }
    }
    return 0;
}
