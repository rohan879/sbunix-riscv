/*
 * basename.c -- strip directory and suffix from filenames.
 *
 * Usage: basename PATH [SUFFIX]
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        dprintf(2, "usage: basename PATH [SUFFIX]\n");
        return 1;
    }

    char *path = argv[1];
    int len = strlen(path);

    /* Strip trailing slashes */
    while (len > 1 && path[len - 1] == '/')
        len--;

    /* Find last slash */
    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') {
            last_slash = i;
            break;
        }
    }

    char *base = path + last_slash + 1;
    int blen = len - (last_slash + 1);

    /* Handle "/" case */
    if (blen == 0) {
        printf("/\n");
        return 0;
    }

    /* Strip suffix if provided */
    if (argc > 2) {
        const char *suffix = argv[2];
        int slen = strlen(suffix);
        if (slen > 0 && blen > slen &&
            memcmp(base + blen - slen, suffix, (size_t)slen) == 0) {
            blen -= slen;
        }
    }

    write(1, base, (size_t)blen);
    write(1, "\n", 1);
    return 0;
}
