/*
 * ln.c -- create hard or symbolic links.
 *
 * Flags: -s (symbolic link).  Default: hard link.
 * Usage: ln [-s] TARGET LINKNAME
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char *argv[]) {
    int opt_symlink = 0;
    int opt;

    while ((opt = getopt(argc, argv, "s")) != -1) {
        switch (opt) {
        case 's': opt_symlink = 1; break;
        default:
            dprintf(2, "usage: ln [-s] TARGET LINKNAME\n");
            return 1;
        }
    }

    if (argc - optind < 2) {
        dprintf(2, "usage: ln [-s] TARGET LINKNAME\n");
        return 1;
    }

    const char *target = argv[optind];
    const char *linkname = argv[optind + 1];

    int r;
    if (opt_symlink)
        r = symlink(target, linkname);
    else
        r = link(target, linkname);

    if (r < 0) {
        dprintf(2, "ln: %s -> %s: %s\n", linkname, target, strerror(errno));
        return 1;
    }
    return 0;
}
