/*
 * tr.c -- translate or delete characters.
 *
 * Usage: tr [-d] SET1 [SET2]
 * Supports ranges like a-z, A-Z, 0-9.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int opt_delete;

/* Expand a set string with ranges (e.g. "a-z") into dst.
 * Returns length of expanded string. */
static int expand_set(const char *s, char *dst, int max) {
    int n = 0;
    for (int i = 0; s[i] && n < max - 1; i++) {
        if (s[i + 1] == '-' && s[i + 2]) {
            char lo = s[i];
            char hi = s[i + 2];
            if (lo <= hi) {
                for (char c = lo; c <= hi && n < max - 1; c++)
                    dst[n++] = c;
            } else {
                for (char c = lo; c >= hi && n < max - 1; c--)
                    dst[n++] = c;
            }
            i += 2;
        } else {
            dst[n++] = s[i];
        }
    }
    dst[n] = '\0';
    return n;
}

int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "d")) != -1) {
        switch (opt) {
        case 'd': opt_delete = 1; break;
        default:
            dprintf(2, "usage: tr [-d] SET1 [SET2]\n");
            return 1;
        }
    }

    if (optind >= argc) {
        dprintf(2, "usage: tr [-d] SET1 [SET2]\n");
        return 1;
    }

    char set1[512], set2[512];
    int len1 = expand_set(argv[optind], set1, sizeof(set1));
    int len2 = 0;

    if (!opt_delete) {
        if (optind + 1 >= argc) {
            dprintf(2, "tr: missing SET2\n");
            return 1;
        }
        len2 = expand_set(argv[optind + 1], set2, sizeof(set2));
    }

    /* Build translation table */
    unsigned char table[256];
    unsigned char del[256];
    memset(del, 0, sizeof(del));
    for (int i = 0; i < 256; i++)
        table[i] = (unsigned char)i;

    if (opt_delete) {
        for (int i = 0; i < len1; i++)
            del[(unsigned char)set1[i]] = 1;
    } else {
        for (int i = 0; i < len1; i++) {
            /* Map set1[i] -> set2[i], or last char of set2 if shorter */
            int j = i < len2 ? i : len2 - 1;
            if (j >= 0)
                table[(unsigned char)set1[i]] = (unsigned char)set2[j];
        }
    }

    /* Process stdin */
    char buf[4096];
    ssize_t r;
    while ((r = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < r; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (opt_delete) {
                if (!del[c])
                    write(STDOUT_FILENO, &buf[i], 1);
            } else {
                unsigned char out = table[c];
                write(STDOUT_FILENO, &out, 1);
            }
        }
    }
    return 0;
}
