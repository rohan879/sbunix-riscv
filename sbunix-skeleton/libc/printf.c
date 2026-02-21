#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

int strlen(const char *s) {
    int i = 0;
    while (s[i]) {
        i++;
    }
    return i;
}

int puts(const char *s) {
    write(1, s, strlen(s));
    write(1, "\n", 1);
    return 0;
}

int getchar(void) {
    char c;
    if (read(0, &c, 1) == 1) {
        return (unsigned char)c;
    }
    return -1;
}

int printf(const char *fmt, ...) {
    va_list ap;
    int n = 0;
    char *s;
    char c;

    va_start(ap, fmt);
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 's':
                    s = va_arg(ap, char *);
                    write(1, s, strlen(s));
                    break;
                case 'c':
                    c = (char)va_arg(ap, int);
                    write(1, &c, 1);
                    break;
                default:
                    write(1, "%", 1);
                    write(1, fmt, 1);
                    break;
            }
        } else {
            write(1, fmt, 1);
        }
        fmt++;
    }
    va_end(ap);
    return n;
}
