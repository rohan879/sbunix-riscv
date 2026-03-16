/*
 * date.c -- print current date/time.
 *
 * Usage: date [+FORMAT]
 * Default format: "%a %b %e %H:%M:%S UTC %Y"
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

int main(int argc, char *argv[]) {
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    char buf[256];
    const char *fmt = "%a %b %e %H:%M:%S UTC %Y";

    if (argc > 1 && argv[1][0] == '+') {
        fmt = argv[1] + 1;
    }

    if (tm == NULL) {
        dprintf(2, "date: cannot get time\n");
        return 1;
    }

    size_t n = strftime(buf, sizeof(buf), fmt, tm);
    if (n == 0) {
        dprintf(2, "date: format string too long\n");
        return 1;
    }

    printf("%s\n", buf);
    return 0;
}
