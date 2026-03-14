/*
 * getopt.c — POSIX command-line option parsing for SBUnix libc.
 *
 * Implements getopt() with the standard optarg, optind, opterr, optopt
 * external variables.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>

char *optarg = (void *)0;
int   optind = 1;
int   opterr = 1;
int   optopt = 0;

/* Internal state: position within current argv element for grouped options */
static int __optpos = 0;

int getopt(int argc, char *const argv[], const char *optstring) {
    const char *p;

    optarg = (void *)0;

    if (optind >= argc)
        return -1;

    /* If we're not in the middle of a grouped option string, advance */
    if (__optpos == 0) {
        /* Check if current arg is an option */
        if (argv[optind] == (void *)0 ||
            argv[optind][0] != '-' ||
            argv[optind][1] == '\0') {
            /* Not an option */
            return -1;
        }

        /* "--" terminates option processing */
        if (argv[optind][1] == '-' && argv[optind][2] == '\0') {
            optind++;
            return -1;
        }

        __optpos = 1; /* start after the '-' */
    }

    /* Get the current option character */
    int c = argv[optind][__optpos];
    __optpos++;

    /* Look up option in optstring */
    /* Skip leading ':' which means caller handles missing-arg */
    p = optstring;
    if (*p == ':')
        p++;

    while (*p) {
        if (*p == c)
            break;
        p++;
        /* Skip the colon(s) after an option letter */
        while (*p == ':')
            p++;
    }

    if (*p == '\0') {
        /* Unknown option */
        optopt = c;
        if (opterr && optstring[0] != ':')
            fprintf(stderr, "%s: unknown option -- '%c'\n",
                    argv[0] ? argv[0] : "", c);

        /* If at end of this argv element, advance */
        if (argv[optind][__optpos] == '\0') {
            optind++;
            __optpos = 0;
        }
        return '?';
    }

    /* Check if option takes an argument */
    if (p[1] == ':') {
        /* Option requires an argument */
        if (argv[optind][__optpos] != '\0') {
            /* Argument is the rest of this argv element */
            optarg = &((char *)argv[optind])[__optpos];
            optind++;
            __optpos = 0;
        } else {
            /* Argument is the next argv element */
            optind++;
            __optpos = 0;
            if (optind >= argc) {
                /* Missing argument */
                optopt = c;
                if (optstring[0] == ':')
                    return ':';
                if (opterr)
                    fprintf(stderr,
                            "%s: option requires an argument -- '%c'\n",
                            argv[0] ? argv[0] : "", c);
                return '?';
            }
            optarg = argv[optind];
            optind++;
        }
    } else {
        /* No argument needed */
        if (argv[optind][__optpos] == '\0') {
            optind++;
            __optpos = 0;
        }
    }

    return c;
}
