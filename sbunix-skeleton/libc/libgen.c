/*
 * libgen.c — basename() and dirname() for SBUnix libc.
 */

#include <libgen.h>
#include <string.h>

char *basename(char *path) {
    static char dot[] = ".";
    if (!path || !*path) return dot;

    /* Strip trailing slashes */
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';

    /* Find last slash */
    char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

char *dirname(char *path) {
    static char dot[] = ".";
    if (!path || !*path) return dot;

    /* Strip trailing slashes */
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';

    /* Find last slash */
    char *p = strrchr(path, '/');
    if (!p) return dot;

    /* Strip trailing slashes from dirname part */
    while (p > path && *p == '/')
        p--;
    *(p + 1) = '\0';
    return path;
}
