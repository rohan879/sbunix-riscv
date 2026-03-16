/*
 * tee.c -- copy stdin to stdout and to files.
 *
 * Flags: -a (append mode).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

int main(int argc, char *argv[]) {
    int opt_append = 0;
    int opt;

    while ((opt = getopt(argc, argv, "a")) != -1) {
        switch (opt) {
        case 'a': opt_append = 1; break;
        default:
            dprintf(2, "usage: tee [-a] [FILE...]\n");
            return 1;
        }
    }

    int nfiles = argc - optind;
    int fds[64];
    int nfds = 0;

    for (int i = optind; i < argc && nfds < 64; i++) {
        int flags = O_WRONLY | O_CREAT;
        flags |= opt_append ? O_APPEND : O_TRUNC;
        int fd = open(argv[i], flags, 0666);
        if (fd < 0) {
            dprintf(2, "tee: %s: %s\n", argv[i], strerror(errno));
            continue;
        }
        fds[nfds++] = fd;
    }

    char buf[4096];
    ssize_t r;
    while ((r = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, (size_t)r);
        for (int i = 0; i < nfds; i++)
            write(fds[i], buf, (size_t)r);
    }

    for (int i = 0; i < nfds; i++)
        close(fds[i]);

    (void)nfiles;
    return 0;
}
