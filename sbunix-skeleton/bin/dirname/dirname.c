/*
 * dirname.c -- strip last component from filename.
 *
 * Usage: dirname PATH
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        dprintf(2, "usage: dirname PATH\n");
        return 1;
    }

    char path[4096];
    strncpy(path, argv[1], sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    int len = strlen(path);

    /* Strip trailing slashes (but not if the entire string is "/") */
    while (len > 1 && path[len - 1] == '/')
        len--;
    path[len] = '\0';

    /* Find last slash */
    char *last = NULL;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') {
            last = &path[i];
            break;
        }
    }

    if (!last) {
        /* No slash: directory is "." */
        printf(".\n");
    } else if (last == path) {
        /* Slash at position 0: directory is "/" */
        printf("/\n");
    } else {
        /* Strip trailing slashes from the directory part */
        while (last > path && *(last - 1) == '/')
            last--;
        *last = '\0';
        printf("%s\n", path);
    }

    return 0;
}
