/*
 * chmod.c -- change file permissions.
 *
 * Usage: chmod MODE FILE...
 * MODE is octal (e.g. 755, 0644).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

static int parse_octal(const char *s, mode_t *mode) {
    unsigned long val = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '7')
            return -1;
        val = val * 8 + (unsigned long)(s[i] - '0');
    }
    *mode = (mode_t)val;
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        dprintf(2, "usage: chmod MODE FILE...\n");
        return 1;
    }

    mode_t mode;
    if (parse_octal(argv[1], &mode) < 0) {
        dprintf(2, "chmod: invalid mode: %s\n", argv[1]);
        return 1;
    }

    int ret = 0;
    for (int i = 2; i < argc; i++) {
        if (chmod(argv[i], mode) < 0) {
            dprintf(2, "chmod: %s: %s\n", argv[i], strerror(errno));
            ret = 1;
        }
    }
    return ret;
}
