#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

static char buf[1024];
static int buf_pos;

static void flush(void) {
    if (buf_pos > 0) {
        write(1, buf, buf_pos);
        buf_pos = 0;
    }
}

static void emit(char c) {
    buf[buf_pos++] = c;
    if (buf_pos >= 1024)
        flush();
}

static void emit_str(const char *s) {
    while (*s)
        emit(*s++);
}

static void emit_unsigned(unsigned long val, int base, int upper) {
    char tmp[20];
    int i = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (val == 0) {
        emit('0');
        return;
    }
    while (val) {
        tmp[i++] = digits[val % base];
        val /= base;
    }
    while (i--)
        emit(tmp[i]);
}

static void emit_signed(long val) {
    if (val < 0) {
        emit('-');
        emit_unsigned(-val, 10, 0);
    } else {
        emit_unsigned(val, 10, 0);
    }
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    buf_pos = 0;

    while (*fmt) {
        if (*fmt != '%') {
            emit(*fmt++);
            continue;
        }
        fmt++;
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }
        switch (*fmt) {
        case 'd':
        case 'i':
            if (is_long)
                emit_signed(va_arg(ap, long));
            else
                emit_signed(va_arg(ap, int));
            break;
        case 'u':
            if (is_long)
                emit_unsigned(va_arg(ap, unsigned long), 10, 0);
            else
                emit_unsigned(va_arg(ap, unsigned int), 10, 0);
            break;
        case 'x':
            if (is_long)
                emit_unsigned(va_arg(ap, unsigned long), 16, 0);
            else
                emit_unsigned(va_arg(ap, unsigned int), 16, 0);
            break;
        case 'X':
            if (is_long)
                emit_unsigned(va_arg(ap, unsigned long), 16, 1);
            else
                emit_unsigned(va_arg(ap, unsigned int), 16, 1);
            break;
        case 'p': {
            unsigned long ptr = (unsigned long)va_arg(ap, void *);
            emit_str("0x");
            emit_unsigned(ptr, 16, 0);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s)
                emit_str(s);
            else
                emit_str("(null)");
            break;
        }
        case 'c':
            emit((char)va_arg(ap, int));
            break;
        case '%':
            emit('%');
            break;
        default:
            emit('%');
            if (is_long)
                emit('l');
            emit(*fmt);
            break;
        }
        fmt++;
    }

    flush();
    va_end(ap);
    return 0;
}

int puts(const char *s) {
    write(1, s, strlen(s));
    write(1, "\n", 1);
    return 0;
}

int putchar(int c) {
    char ch = (char)c;
    write(1, &ch, 1);
    return c;
}

int getchar(void) {
    char c;
    if (read(0, &c, 1) == 1)
        return (unsigned char)c;
    return -1;
}
