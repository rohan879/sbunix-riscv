/*
 * getopt.c — POSIX getopt() for SBUnix libc.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>

char *optarg = NULL;
int   optind = 1;
int   opterr = 1;
int   optopt = 0;

int getopt(int argc, char *const argv[], const char *optstring) {
    static int sp = 1; /* position within current argv element */

    if (optind >= argc || argv[optind] == NULL)
        return -1;

    char *arg = argv[optind];

    if (arg[0] != '-' || arg[1] == '\0')
        return -1;

    if (arg[1] == '-' && arg[2] == '\0') {
        /* "--" terminates options */
        optind++;
        return -1;
    }

    char c = arg[sp];
    const char *cp = strchr(optstring, c);

    if (c == ':' || cp == NULL) {
        optopt = c;
        if (opterr)
            dprintf(2, "%s: illegal option -- %c\n", argv[0], c);
        if (arg[++sp] == '\0') {
            optind++;
            sp = 1;
        }
        return '?';
    }

    if (cp[1] == ':') {
        /* Option requires an argument */
        if (arg[sp + 1] != '\0') {
            /* Argument is rest of current argv element */
            optarg = &arg[sp + 1];
        } else if (optind + 1 < argc) {
            /* Argument is next argv element */
            optarg = argv[++optind];
        } else {
            optopt = c;
            if (opterr)
                dprintf(2, "%s: option requires an argument -- %c\n", argv[0], c);
            optind++;
            sp = 1;
            return (optstring[0] == ':') ? ':' : '?';
        }
        optind++;
        sp = 1;
    } else {
        /* No argument */
        optarg = NULL;
        if (arg[++sp] == '\0') {
            optind++;
            sp = 1;
        }
    }

    return c;
}
