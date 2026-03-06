#ifndef _STDIO_H
#define _STDIO_H

#include <sys/types.h>
#include <stdarg.h>

#define EOF (-1)

/* Formatted output to stdout */
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);

/* Formatted output to a string buffer */
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* Formatted output to a file descriptor */
int dprintf(int fd, const char *fmt, ...);
int vdprintf(int fd, const char *fmt, va_list ap);

/* Basic I/O */
int puts(const char *s);
int putchar(int c);
int getchar(void);

#endif /* _STDIO_H */
