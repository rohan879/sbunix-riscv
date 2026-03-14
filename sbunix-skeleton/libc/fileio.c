/*
 * fileio.c — FILE* stream implementation for SBUnix libc.
 *
 * Provides fprintf, perror, fopen, fclose, fread, fwrite, etc.
 * Streams support buffered writes (_IOFBF, _IOLBF, _IONBF).
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

/* Static FILE objects for stdin, stdout, stderr */
static FILE __stdin  = { .fd = 0, .eof = 0, .error = 0, .buf_mode = _IONBF, .wpos = 0, .ungot = -1 };
static FILE __stdout = { .fd = 1, .eof = 0, .error = 0, .buf_mode = _IOLBF, .wpos = 0, .ungot = -1 };
static FILE __stderr = { .fd = 2, .eof = 0, .error = 0, .buf_mode = _IONBF, .wpos = 0, .ungot = -1 };

FILE *stdin  = &__stdin;
FILE *stdout = &__stdout;
FILE *stderr = &__stderr;

/* Static pool of FILE objects for fopen */
#define FILE_POOL_SIZE 16
static FILE __file_pool[FILE_POOL_SIZE];
static int  __file_pool_used[FILE_POOL_SIZE];

/* -----------------------------------------------------------------------
 * Stream registry — tracks all open FILE* for fflush(NULL)
 * ---------------------------------------------------------------------- */
#define STREAM_REG_SIZE (3 + FILE_POOL_SIZE)
static FILE *__open_streams[STREAM_REG_SIZE];
static int   __streams_init;

void __stdio_after_fork_child(void) {
    __stdin.eof = 0;
    __stdin.error = 0;
    __stdin.wpos = 0;
    __stdin.ungot = -1;

    __stdout.eof = 0;
    __stdout.error = 0;
    __stdout.wpos = 0;
    __stdout.ungot = -1;

    __stderr.eof = 0;
    __stderr.error = 0;
    __stderr.wpos = 0;
    __stderr.ungot = -1;

    for (int i = 0; i < FILE_POOL_SIZE; i++) {
        __file_pool[i].eof = 0;
        __file_pool[i].error = 0;
        __file_pool[i].wpos = 0;
        __file_pool[i].ungot = -1;
    }

    memset(__open_streams, 0, sizeof(__open_streams));
    __open_streams[0] = stdin;
    __open_streams[1] = stdout;
    __open_streams[2] = stderr;
    __streams_init = 1;
}

static void __stream_register(FILE *stream) {
    if (!__streams_init) {
        __open_streams[0] = stdin;
        __open_streams[1] = stdout;
        __open_streams[2] = stderr;
        __streams_init = 1;
    }
    for (int i = 0; i < STREAM_REG_SIZE; i++) {
        if (__open_streams[i] == stream) return; /* already registered */
    }
    for (int i = 0; i < STREAM_REG_SIZE; i++) {
        if (!__open_streams[i]) {
            __open_streams[i] = stream;
            return;
        }
    }
}

static void __stream_unregister(FILE *stream) {
    for (int i = 0; i < STREAM_REG_SIZE; i++) {
        if (__open_streams[i] == stream) {
            __open_streams[i] = (void *)0;
            return;
        }
    }
}

/* -----------------------------------------------------------------------
 * Internal flush helper — writes pending buffer contents via write()
 * ---------------------------------------------------------------------- */
static int __fflush_stream(FILE *stream) {
    if (!stream || stream->wpos == 0) return 0;
    ssize_t r = write(stream->fd, stream->wbuf, stream->wpos);
    stream->wpos = 0;
    if (r < 0) {
        stream->error = 1;
        return EOF;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Buffered character output
 * ---------------------------------------------------------------------- */
int fputc(int c, FILE *stream) {
    unsigned char ch = (unsigned char)c;

    if (stream->buf_mode == _IONBF) {
        /* Unbuffered: write immediately */
        ssize_t r = write(stream->fd, &ch, 1);
        if (r < 0) { stream->error = 1; return EOF; }
        return ch;
    }

    /* Buffer the character */
    stream->wbuf[stream->wpos++] = (char)ch;

    if (stream->buf_mode == _IOLBF) {
        /* Line-buffered: flush on newline or when full */
        if (ch == '\n' || stream->wpos >= FILE_BUF_SIZE)
            if (__fflush_stream(stream) == EOF) return EOF;
    } else {
        /* Fully buffered: flush when full */
        if (stream->wpos >= FILE_BUF_SIZE)
            if (__fflush_stream(stream) == EOF) return EOF;
    }

    return ch;
}

int fputs(const char *s, FILE *stream) {
    while (*s) {
        if (fputc((unsigned char)*s++, stream) == EOF)
            return EOF;
    }
    return 0;
}

int fprintf(FILE *stream, const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    int w = n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1;
    for (int i = 0; i < w; i++) {
        if (fputc((unsigned char)tmp[i], stream) == EOF)
            return EOF;
    }
    return n;
}

int vfprintf(FILE *stream, const char *fmt, va_list ap) {
    char tmp[4096];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    int w = n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1;
    for (int i = 0; i < w; i++) {
        if (fputc((unsigned char)tmp[i], stream) == EOF)
            return EOF;
    }
    return n;
}

void perror(const char *s) {
    if (s && *s)
        fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else
        fprintf(stderr, "%s\n", strerror(errno));
}

char *fgets(char *s, int size, FILE *stream) {
    if (size <= 0) return (void *)0;
    int i = 0;
    while (i < size - 1) {
        int ch = fgetc(stream);
        if (ch == EOF)
            break;
        s[i++] = (char)ch;
        if (ch == '\n') break;
    }
    if (i == 0) return (void *)0;
    s[i] = '\0';
    return s;
}

int fgetc(FILE *stream) {
    /* Return ungot character if available */
    if (stream->ungot >= 0) {
        int c = stream->ungot;
        stream->ungot = -1;
        stream->eof = 0;
        return c;
    }
    unsigned char c;
    ssize_t r = read(stream->fd, &c, 1);
    if (r < 0) {
        stream->error = 1;
        return EOF;
    }
    if (r == 0) {
        stream->eof = 1;
        return EOF;
    }
    return (int)c;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (size == 0 || nmemb == 0) return 0;
    /* Guard against multiplication overflow */
    if (nmemb > SIZE_MAX / size) return 0;
    size_t total = size * nmemb;
    const unsigned char *p = (const unsigned char *)ptr;
    for (size_t i = 0; i < total; i++) {
        if (fputc(p[i], stream) == EOF)
            return i / size;
    }
    return nmemb;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t total = size * nmemb;
    if (total == 0) return 0;
    ssize_t r = read(stream->fd, ptr, total);
    if (r < 0) {
        stream->error = 1;
        return 0;
    }
    if (r == 0) {
        stream->eof = 1;
        return 0;
    }
    return (size_t)r / size;
}

FILE *fopen(const char *path, const char *mode) {
    int flags = 0;
    if (mode[0] == 'r') {
        if (mode[1] == '+') flags = O_RDWR;
        else flags = O_RDONLY;
    } else if (mode[0] == 'w') {
        if (mode[1] == '+') flags = O_RDWR | O_CREAT | O_TRUNC;
        else flags = O_WRONLY | O_CREAT | O_TRUNC;
    } else if (mode[0] == 'a') {
        if (mode[1] == '+') flags = O_RDWR | O_CREAT | O_APPEND;
        else flags = O_WRONLY | O_CREAT | O_APPEND;
    } else {
        return (void *)0;
    }

    int fd = open(path, flags, 0666);
    if (fd < 0) return (void *)0;

    /* Find a free slot in the pool */
    for (int i = 0; i < FILE_POOL_SIZE; i++) {
        if (!__file_pool_used[i]) {
            __file_pool_used[i] = 1;
            __file_pool[i].fd = fd;
            __file_pool[i].eof = 0;
            __file_pool[i].error = 0;
            __file_pool[i].buf_mode = _IOFBF;
            __file_pool[i].wpos = 0;
            __file_pool[i].ungot = -1;
            __stream_register(&__file_pool[i]);
            return &__file_pool[i];
        }
    }

    /* No free slots */
    close(fd);
    errno = EMFILE;
    return (void *)0;
}

int fclose(FILE *stream) {
    if (!stream) return EOF;
    __fflush_stream(stream);
    __stream_unregister(stream);
    int ret = close(stream->fd);
    /* Return slot to pool if it's from the pool */
    for (int i = 0; i < FILE_POOL_SIZE; i++) {
        if (&__file_pool[i] == stream) {
            __file_pool_used[i] = 0;
            break;
        }
    }
    return ret;
}

int fflush(FILE *stream) {
    if (stream)
        return __fflush_stream(stream);
    /* fflush(NULL): flush all registered streams */
    if (!__streams_init) {
        __open_streams[0] = stdin;
        __open_streams[1] = stdout;
        __open_streams[2] = stderr;
        __streams_init = 1;
    }
    int ret = 0;
    for (int i = 0; i < STREAM_REG_SIZE; i++) {
        if (__open_streams[i]) {
            if (__fflush_stream(__open_streams[i]) == EOF)
                ret = EOF;
        }
    }
    return ret;
}

int fileno(FILE *stream) {
    return stream->fd;
}

int feof(FILE *stream) {
    return stream->eof;
}

int ferror(FILE *stream) {
    return stream->error;
}

void clearerr(FILE *stream) {
    stream->eof = 0;
    stream->error = 0;
}

int fseek(FILE *stream, long offset, int whence) {
    off_t r = lseek(stream->fd, offset, whence);
    if (r < 0) return -1;
    stream->eof = 0;
    stream->ungot = -1;
    return 0;
}

long ftell(FILE *stream) {
    return (long)lseek(stream->fd, 0, SEEK_CUR);
}

void rewind(FILE *stream) {
    lseek(stream->fd, 0, SEEK_SET);
    stream->eof = 0;
    stream->error = 0;
    stream->ungot = -1;
}

/*
 * sscanf -- parse formatted input from a string.
 * Supports: %d %u %x %ld %lu %lx %s %c %n %% %[...] %*<spec> and width.
 */
int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const char *s = str;
    int assignments = 0;

    while (*fmt) {
        /* Whitespace in format: skip whitespace in input */
        if (*fmt == ' ' || *fmt == '\t' || *fmt == '\n') {
            while (*s == ' ' || *s == '\t' || *s == '\n' ||
                   *s == '\r' || *s == '\f' || *s == '\v')
                s++;
            fmt++;
            continue;
        }

        /* Literal character match */
        if (*fmt != '%') {
            if (*s != *fmt)
                break;
            s++;
            fmt++;
            continue;
        }

        fmt++; /* skip '%' */

        /* %% -- literal percent */
        if (*fmt == '%') {
            if (*s != '%')
                break;
            s++;
            fmt++;
            continue;
        }

        /* Assignment suppression flag */
        int suppress = 0;
        if (*fmt == '*') {
            suppress = 1;
            fmt++;
        }

        /* Width specifier */
        int width = 0;
        int has_width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            has_width = 1;
            fmt++;
        }

        /* Length modifier */
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l') {
                /* 'll' -- treat same as 'l' on LP64 */
                fmt++;
            }
        }

        char spec = *fmt++;

        switch (spec) {
        case 'd': {
            /* Skip whitespace */
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;
            if (!*s) goto done;
            int neg = 0;
            const char *start = s;
            if (*s == '-') { neg = 1; s++; }
            else if (*s == '+') { s++; }
            if (*s < '0' || *s > '9') { s = start; goto done; }
            long val = 0;
            int count = 0;
            while (*s >= '0' && *s <= '9') {
                if (has_width && count >= width) break;
                val = val * 10 + (*s - '0');
                s++;
                count++;
            }
            if (neg) val = -val;
            if (!suppress) {
                if (is_long)
                    *va_arg(ap, long *) = val;
                else
                    *va_arg(ap, int *) = (int)val;
                assignments++;
            }
            break;
        }
        case 'u': {
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;
            if (!*s) goto done;
            const char *start = s;
            if (*s == '+') s++;
            if (*s < '0' || *s > '9') { s = start; goto done; }
            unsigned long val = 0;
            int count = 0;
            while (*s >= '0' && *s <= '9') {
                if (has_width && count >= width) break;
                val = val * 10 + (*s - '0');
                s++;
                count++;
            }
            if (!suppress) {
                if (is_long)
                    *va_arg(ap, unsigned long *) = val;
                else
                    *va_arg(ap, unsigned int *) = (unsigned int)val;
                assignments++;
            }
            break;
        }
        case 'x': case 'X': {
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;
            if (!*s) goto done;
            const char *start = s;
            if (*s == '+') s++;
            /* Skip optional 0x prefix */
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                s += 2;
            if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') ||
                  (*s >= 'A' && *s <= 'F'))) { s = start; goto done; }
            unsigned long val = 0;
            int count = 0;
            for (;;) {
                if (has_width && count >= width) break;
                int digit;
                if (*s >= '0' && *s <= '9') digit = *s - '0';
                else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
                else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
                else break;
                val = val * 16 + digit;
                s++;
                count++;
            }
            if (!suppress) {
                if (is_long)
                    *va_arg(ap, unsigned long *) = val;
                else
                    *va_arg(ap, unsigned int *) = (unsigned int)val;
                assignments++;
            }
            break;
        }
        case 's': {
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;
            if (!*s) goto done;
            char *dst = suppress ? (char *)0 : va_arg(ap, char *);
            int count = 0;
            while (*s && *s != ' ' && *s != '\t' && *s != '\n' &&
                   *s != '\r' && *s != '\f' && *s != '\v') {
                if (has_width && count >= width) break;
                if (dst) dst[count] = *s;
                s++;
                count++;
            }
            if (count == 0) goto done;
            if (dst) dst[count] = '\0';
            if (!suppress) assignments++;
            break;
        }
        case 'c': {
            if (!*s) goto done;
            int n = has_width ? width : 1;
            if (n == 0) n = 1;
            char *dst = suppress ? (char *)0 : va_arg(ap, char *);
            for (int ci = 0; ci < n; ci++) {
                if (!*s) goto done;
                if (dst) dst[ci] = *s;
                s++;
            }
            if (!suppress) assignments++;
            break;
        }
        case 'n': {
            if (!suppress)
                *va_arg(ap, int *) = (int)(s - str);
            /* %n does not count as an assignment */
            break;
        }
        case '[': {
            /* Scanset: %[...] */
            int invert = 0;
            if (*fmt == '^') {
                invert = 1;
                fmt++;
            }
            /* Build set of accepted characters */
            char set[256];
            for (int si = 0; si < 256; si++) set[si] = 0;

            /* Handle ] as first char in set */
            if (*fmt == ']') {
                set[(unsigned char)']'] = 1;
                fmt++;
            }
            while (*fmt && *fmt != ']') {
                /* Range: a-z */
                if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') {
                    unsigned char lo = (unsigned char)fmt[0];
                    unsigned char hi = (unsigned char)fmt[2];
                    for (unsigned c2 = lo; c2 <= hi; c2++)
                        set[c2] = 1;
                    fmt += 3;
                } else {
                    set[(unsigned char)*fmt] = 1;
                    fmt++;
                }
            }
            if (*fmt == ']') fmt++; /* skip closing ] */

            char *dst = suppress ? (char *)0 : va_arg(ap, char *);
            int count = 0;
            while (*s) {
                if (has_width && count >= width) break;
                int in_set = set[(unsigned char)*s];
                if (invert) in_set = !in_set;
                if (!in_set) break;
                if (dst) dst[count] = *s;
                s++;
                count++;
            }
            if (count == 0) goto done;
            if (dst) dst[count] = '\0';
            if (!suppress) assignments++;
            break;
        }
        default:
            /* Unknown specifier -- stop */
            goto done;
        }
    }

done:
    va_end(ap);
    return assignments;
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size) {
    (void)buf; (void)size;
    if (mode != _IONBF && mode != _IOLBF && mode != _IOFBF)
        return -1;
    __fflush_stream(stream);
    stream->buf_mode = mode;
    return 0;
}

void setbuf(FILE *stream, char *buf) {
    (void)stream; (void)buf;
}

int ungetc(int c, FILE *stream) {
    if (c == EOF)
        return EOF;
    stream->ungot = (unsigned char)c;
    stream->eof = 0;
    return (unsigned char)c;
}

int getc(FILE *stream) {
    return fgetc(stream);
}

int putc(int c, FILE *stream) {
    return fputc(c, stream);
}

/*
 * fdopen() — associate a FILE stream with an existing file descriptor.
 */
FILE *fdopen(int fd, const char *mode) {
    (void)mode; /* mode is advisory; fd is already open with its flags */
    if (fd < 0) {
        errno = EBADF;
        return (void *)0;
    }

    /* Find a free slot in the pool */
    for (int i = 0; i < FILE_POOL_SIZE; i++) {
        if (!__file_pool_used[i]) {
            __file_pool_used[i] = 1;
            __file_pool[i].fd = fd;
            __file_pool[i].eof = 0;
            __file_pool[i].error = 0;
            __file_pool[i].buf_mode = _IOFBF;
            __file_pool[i].wpos = 0;
            __file_pool[i].ungot = -1;
            __stream_register(&__file_pool[i]);
            return &__file_pool[i];
        }
    }

    errno = EMFILE;
    return (void *)0;
}

/*
 * tmpfile() — create and open a temporary file.
 * The file is unlinked immediately so it vanishes when closed.
 */
FILE *tmpfile(void) {
    char tmpl[] = "/tmp/tmpXXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0)
        return (void *)0;

    /* Unlink immediately so the file disappears when closed */
    unlink(tmpl);

    FILE *fp = fdopen(fd, "w+");
    if (!fp) {
        close(fd);
        return (void *)0;
    }
    return fp;
}
