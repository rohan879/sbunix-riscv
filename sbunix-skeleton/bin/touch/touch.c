/*
 * touch — create files or update timestamps.
 *
 * Usage: touch FILE [FILE ...]
 *
 * Creates each FILE if it does not exist.  If it does exist, this
 * implementation is a no-op (the kernel has no utimensat support, so
 * timestamp updating is not possible).
 *
 * Files can only be created on writable filesystems (e.g. /tmp).
 * Attempting to touch a path under the read-only tarfs root will fail
 * with an error message and a non-zero exit status.
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        dprintf(2, "usage: touch FILE [FILE ...]\n");
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        /* Skip option flags (e.g. -c, -m, -a) silently */
        if (argv[i][0] == '-')
            continue;

        /*
         * O_CREAT | O_WRONLY: create if absent.
         * No O_TRUNC: preserve existing content.
         */
        int fd = open(argv[i], O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            dprintf(2, "touch: %s: cannot create: %d\n", argv[i], errno);
            rc = 1;
            continue;
        }
        close(fd);
    }
    return rc;
}
