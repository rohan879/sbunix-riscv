/*
 * uniq.c -- filter adjacent duplicate lines.
 *
 * Flags: -c (prefix count), -d (only duplicates), -u (only unique).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int opt_count, opt_dup_only, opt_uniq_only;

static void flush_line(const char *line, int count) {
    if (opt_dup_only && count < 2)
        return;
    if (opt_uniq_only && count > 1)
        return;
    if (opt_count)
        printf("%7d %s\n", count, line);
    else
        printf("%s\n", line);
}

static void uniq_fd(int fd) {
    char buf[4096];
    char prev[4096];
    char line[4096];
    int lpos = 0;
    int have_prev = 0;
    int count = 0;
    ssize_t r;

    prev[0] = '\0';

    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n') {
                line[lpos] = '\0';
                if (!have_prev) {
                    strcpy(prev, line);
                    count = 1;
                    have_prev = 1;
                } else if (strcmp(line, prev) == 0) {
                    count++;
                } else {
                    flush_line(prev, count);
                    strcpy(prev, line);
                    count = 1;
                }
                lpos = 0;
            } else if (lpos < (int)sizeof(line) - 1) {
                line[lpos++] = buf[i];
            }
        }
    }
    /* Handle last line without trailing newline */
    if (lpos > 0) {
        line[lpos] = '\0';
        if (!have_prev) {
            strcpy(prev, line);
            count = 1;
            have_prev = 1;
        } else if (strcmp(line, prev) == 0) {
            count++;
        } else {
            flush_line(prev, count);
            strcpy(prev, line);
            count = 1;
        }
    }
    if (have_prev)
        flush_line(prev, count);
}

int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "cdu")) != -1) {
        switch (opt) {
        case 'c': opt_count = 1; break;
        case 'd': opt_dup_only = 1; break;
        case 'u': opt_uniq_only = 1; break;
        default:
            dprintf(2, "usage: uniq [-cdu] [FILE]\n");
            return 1;
        }
    }

    if (optind < argc) {
        int fd = open(argv[optind], O_RDONLY);
        if (fd < 0) {
            dprintf(2, "uniq: %s: %s\n", argv[optind], strerror(errno));
            return 1;
        }
        uniq_fd(fd);
        close(fd);
    } else {
        uniq_fd(STDIN_FILENO);
    }
    return 0;
}
