/*
 * fileio.c — FILE* stream implementation for SBUnix libc.
 *
 * Provides fprintf, perror, fopen, fclose, fread, fwrite, etc.
 * Streams support buffered writes (_IOFBF, _IOLBF, _IONBF).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>

/* Static FILE objects for stdin, stdout, stderr */
static FILE __stdin  = { .fd = 0, .eof = 0, .error = 0, .buf_mode = _IONBF, .wpos = 0 };
static FILE __stdout = { .fd = 1, .eof = 0, .error = 0, .buf_mode = _IOLBF, .wpos = 0 };
static FILE __stderr = { .fd = 2, .eof = 0, .error = 0, .buf_mode = _IONBF, .wpos = 0 };

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
    char tmp[1024];
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
    char tmp[1024];
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
        unsigned char c;
        ssize_t r = read(stream->fd, &c, 1);
        if (r < 0) {
            stream->error = 1;
            break;
        }
        if (r == 0) {
            stream->eof = 1;
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return (void *)0;
    s[i] = '\0';
    return s;
}

int fgetc(FILE *stream) {
    if (stream->has_unget) {
        stream->has_unget = 0;
        return stream->unget_char;
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
    size_t total = size * nmemb;
    if (total == 0) return 0;
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
            __file_pool[i].unget_char = 0;
            __file_pool[i].has_unget = 0;
            __stream_register(&__file_pool[i]);
            return &__file_pool[i];
        }
    }

    /* No free slots */
    close(fd);
    errno = EMFILE;
    return (void *)0;
}

FILE *fdopen(int fd, const char *mode) {
    if (fd < 0 || !mode) return (void *)0;

    /* Determine buffer mode from the mode string (default fully buffered) */
    int buf_mode = _IOFBF;
    (void)mode; /* mode is accepted for API compat; fd is already open */

    /* Find a free slot in the pool */
    for (int i = 0; i < FILE_POOL_SIZE; i++) {
        if (!__file_pool_used[i]) {
            __file_pool_used[i] = 1;
            __file_pool[i].fd = fd;
            __file_pool[i].eof = 0;
            __file_pool[i].error = 0;
            __file_pool[i].buf_mode = buf_mode;
            __file_pool[i].wpos = 0;
            __file_pool[i].unget_char = 0;
            __file_pool[i].has_unget = 0;
            __stream_register(&__file_pool[i]);
            return &__file_pool[i];
        }
    }

    errno = EMFILE;
    return (void *)0;
}

FILE *tmpfile(void) {
    char name[] = "/tmp/tmpXXXXXX";
    int fd = mkstemp(name);
    if (fd < 0)
        return (void *)0;
    unlink(name); /* delete-on-close semantics */
    FILE *fp = fdopen(fd, "w+");
    if (!fp) {
        close(fd);
        return (void *)0;
    }
    return fp;
}

FILE *freopen(const char *path, const char *mode, FILE *stream) {
    if (!stream) return (void *)0;
    __fflush_stream(stream);
    close(stream->fd);

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

    int fd;
    if (path) {
        fd = open(path, flags, 0666);
    } else {
        /* NULL path: reopen same fd with new mode (not fully supported) */
        return (void *)0;
    }
    if (fd < 0) return (void *)0;

    stream->fd = fd;
    stream->eof = 0;
    stream->error = 0;
    stream->wpos = 0;
    stream->has_unget = 0;
    return stream;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    if (!lineptr || !n || !stream) {
        errno = EINVAL;
        return -1;
    }
    if (*lineptr == (void *)0 || *n == 0) {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) return -1;
    }
    size_t pos = 0;
    int c;
    while ((c = fgetc(stream)) != EOF) {
        if (pos + 2 > *n) {
            size_t newn = *n * 2;
            char *newp = realloc(*lineptr, newn);
            if (!newp) return -1;
            *lineptr = newp;
            *n = newn;
        }
        (*lineptr)[pos++] = (char)c;
        if (c == '\n') break;
    }
    if (pos == 0) return -1;
    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream) {
    if (!lineptr || !n || !stream) {
        errno = EINVAL;
        return -1;
    }
    if (*lineptr == (void *)0 || *n == 0) {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) return -1;
    }
    size_t pos = 0;
    int c;
    while ((c = fgetc(stream)) != EOF) {
        if (pos + 2 > *n) {
            size_t newn = *n * 2;
            char *newp = realloc(*lineptr, newn);
            if (!newp) return -1;
            *lineptr = newp;
            *n = newn;
        }
        (*lineptr)[pos++] = (char)c;
        if (c == delim) break;
    }
    if (pos == 0) return -1;
    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
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
    return 0;
}

long ftell(FILE *stream) {
    return (long)lseek(stream->fd, 0, SEEK_CUR);
}

void rewind(FILE *stream) {
    lseek(stream->fd, 0, SEEK_SET);
    stream->eof = 0;
    stream->error = 0;
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    const char *s = str;   /* current position in input */
    const char *f = fmt;   /* current position in format */
    int matched = 0;       /* number of items assigned */

    /* Local helpers — avoids adding #include <ctype.h> */
#define SS_ISSPACE(c) ((c)==' '||(c)=='\t'||(c)=='\n'||(c)=='\r'||(c)=='\f'||(c)=='\v')
#define SS_ISDIGIT(c) ((c)>='0'&&(c)<='9')
#define SS_ISXDIGIT(c) (SS_ISDIGIT(c)||((c)>='a'&&(c)<='f')||((c)>='A'&&(c)<='F'))
#define SS_ISODIGIT(c) ((c)>='0'&&(c)<='7')

    while (*f) {
        /* Whitespace in format: skip any whitespace in input */
        if (SS_ISSPACE((unsigned char)*f)) {
            while (SS_ISSPACE((unsigned char)*s))
                s++;
            while (SS_ISSPACE((unsigned char)*f))
                f++;
            continue;
        }

        /* Literal character (non-%) */
        if (*f != '%') {
            if (*s != *f)
                goto done;
            s++;
            f++;
            continue;
        }

        /* We have '%' */
        f++; /* skip '%' */

        /* %% — literal percent */
        if (*f == '%') {
            if (*s != '%')
                goto done;
            s++;
            f++;
            continue;
        }

        /* Parse optional '*' suppression */
        int suppress = 0;
        if (*f == '*') {
            suppress = 1;
            f++;
        }

        /* Parse optional width */
        int width = 0;
        int has_width = 0;
        while (SS_ISDIGIT((unsigned char)*f)) {
            width = width * 10 + (*f - '0');
            has_width = 1;
            f++;
        }

        /* Parse length modifier */
        int length = 0; /* 0=int, 1=long, 2=long long */
        if (*f == 'l') {
            f++;
            length = 1;
            if (*f == 'l') {
                f++;
                length = 2;
            }
        } else if (*f == 'h') {
            f++;
            length = -1; /* short */
            if (*f == 'h') {
                f++;
                length = -2; /* char */
            }
        }

        char spec = *f++;

        switch (spec) {
        case 'd': case 'i': {
            /* Skip leading whitespace */
            while (SS_ISSPACE((unsigned char)*s))
                s++;
            if (!*s)
                goto done;

            const char *start = s;
            int neg = 0;
            if (*s == '-' || *s == '+') {
                neg = (*s == '-');
                s++;
                if (has_width) width--;
            }
            if (!SS_ISDIGIT((unsigned char)*s)) {
                s = start; /* revert */
                goto done;
            }

            long long val = 0;
            int count = 0;
            while (SS_ISDIGIT((unsigned char)*s) && (!has_width || count < width)) {
                val = val * 10 + (*s - '0');
                s++;
                count++;
            }
            if (neg) val = -val;

            if (!suppress) {
                if (length == 2)
                    *va_arg(ap, long long *) = val;
                else if (length == 1)
                    *va_arg(ap, long *) = (long)val;
                else if (length == -1)
                    *va_arg(ap, short *) = (short)val;
                else if (length == -2)
                    *va_arg(ap, signed char *) = (signed char)val;
                else
                    *va_arg(ap, int *) = (int)val;
                matched++;
            }
            break;
        }
        case 'u': {
            while (SS_ISSPACE((unsigned char)*s))
                s++;
            if (!*s)
                goto done;

            const char *start = s;
            if (*s == '+') {
                s++;
                if (has_width) width--;
            }
            if (!SS_ISDIGIT((unsigned char)*s)) {
                s = start;
                goto done;
            }

            unsigned long long val = 0;
            int count = 0;
            while (SS_ISDIGIT((unsigned char)*s) && (!has_width || count < width)) {
                val = val * 10 + (unsigned)(*s - '0');
                s++;
                count++;
            }

            if (!suppress) {
                if (length == 2)
                    *va_arg(ap, unsigned long long *) = val;
                else if (length == 1)
                    *va_arg(ap, unsigned long *) = (unsigned long)val;
                else if (length == -1)
                    *va_arg(ap, unsigned short *) = (unsigned short)val;
                else if (length == -2)
                    *va_arg(ap, unsigned char *) = (unsigned char)val;
                else
                    *va_arg(ap, unsigned int *) = (unsigned int)val;
                matched++;
            }
            break;
        }
        case 'x': case 'X': {
            while (SS_ISSPACE((unsigned char)*s))
                s++;
            if (!*s)
                goto done;

            const char *start = s;
            /* Skip optional 0x/0X prefix */
            if (*s == '0' && (*(s+1) == 'x' || *(s+1) == 'X')) {
                s += 2;
                if (has_width) width -= 2;
            }
            if (!SS_ISXDIGIT((unsigned char)*s)) {
                s = start;
                goto done;
            }

            unsigned long long val = 0;
            int count = 0;
            while (SS_ISXDIGIT((unsigned char)*s) && (!has_width || count < width)) {
                unsigned char c = (unsigned char)*s;
                unsigned digit;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                else digit = c - 'A' + 10;
                val = val * 16 + digit;
                s++;
                count++;
            }

            if (!suppress) {
                if (length == 2)
                    *va_arg(ap, unsigned long long *) = val;
                else if (length == 1)
                    *va_arg(ap, unsigned long *) = (unsigned long)val;
                else
                    *va_arg(ap, unsigned int *) = (unsigned int)val;
                matched++;
            }
            break;
        }
        case 'o': {
            while (SS_ISSPACE((unsigned char)*s))
                s++;
            if (!*s)
                goto done;

            if (!SS_ISODIGIT((unsigned char)*s))
                goto done;

            unsigned long long val = 0;
            int count = 0;
            while (SS_ISODIGIT((unsigned char)*s) && (!has_width || count < width)) {
                val = val * 8 + (unsigned)(*s - '0');
                s++;
                count++;
            }

            if (!suppress) {
                if (length == 2)
                    *va_arg(ap, unsigned long long *) = val;
                else if (length == 1)
                    *va_arg(ap, unsigned long *) = (unsigned long)val;
                else
                    *va_arg(ap, unsigned int *) = (unsigned int)val;
                matched++;
            }
            break;
        }
        case 's': {
            while (SS_ISSPACE((unsigned char)*s))
                s++;
            if (!*s)
                goto done;

            int count = 0;
            if (!suppress) {
                char *dest = va_arg(ap, char *);
                while (*s && !SS_ISSPACE((unsigned char)*s) &&
                       (!has_width || count < width)) {
                    *dest++ = *s++;
                    count++;
                }
                *dest = '\0';
                matched++;
            } else {
                while (*s && !SS_ISSPACE((unsigned char)*s) &&
                       (!has_width || count < width)) {
                    s++;
                    count++;
                }
            }
            break;
        }
        case 'c': {
            int w = has_width ? width : 1;
            if (!suppress) {
                char *dest = va_arg(ap, char *);
                int i;
                for (i = 0; i < w; i++) {
                    if (!*s)
                        goto done;
                    *dest++ = *s++;
                }
                matched++;
            } else {
                int i;
                for (i = 0; i < w; i++) {
                    if (!*s)
                        goto done;
                    s++;
                }
            }
            break;
        }
        case 'n': {
            if (!suppress) {
                if (length == 2)
                    *va_arg(ap, long long *) = (long long)(s - str);
                else if (length == 1)
                    *va_arg(ap, long *) = (long)(s - str);
                else
                    *va_arg(ap, int *) = (int)(s - str);
            }
            /* %n does not count toward matched */
            break;
        }
        case '[': {
            /* Character class matching: %[...] and %[^...] */
            int negate = 0;
            if (*f == '^') {
                negate = 1;
                f++;
            }

            /* Build a 256-bit set */
            unsigned char charset[256 / 8];
            {
                int k;
                for (k = 0; k < (int)(sizeof(charset)); k++)
                    charset[k] = 0;
            }

            /* ']' as first char (after optional '^') is literal */
            if (*f == ']') {
                charset[(unsigned char)']' / 8] |= (unsigned char)(1 << ((unsigned char)']' % 8));
                f++;
            }
            while (*f && *f != ']') {
                unsigned char lo = (unsigned char)*f;
                if (*(f+1) == '-' && *(f+2) && *(f+2) != ']') {
                    /* range lo-hi */
                    unsigned char hi = (unsigned char)*(f+2);
                    unsigned char c;
                    for (c = lo; c <= hi; c++)
                        charset[c / 8] |= (unsigned char)(1 << (c % 8));
                    f += 3;
                } else {
                    charset[lo / 8] |= (unsigned char)(1 << (lo % 8));
                    f++;
                }
            }
            if (*f == ']')
                f++;

            /* Now scan the input */
            int count = 0;
            if (!suppress) {
                char *dest = va_arg(ap, char *);
                while (*s && (!has_width || count < width)) {
                    unsigned char c = (unsigned char)*s;
                    int in_set = (charset[c / 8] >> (c % 8)) & 1;
                    if (negate ? in_set : !in_set)
                        break;
                    *dest++ = *s++;
                    count++;
                }
                *dest = '\0';
            } else {
                while (*s && (!has_width || count < width)) {
                    unsigned char c = (unsigned char)*s;
                    int in_set = (charset[c / 8] >> (c % 8)) & 1;
                    if (negate ? in_set : !in_set)
                        break;
                    s++;
                    count++;
                }
            }
            if (count == 0)
                goto done;
            if (!suppress)
                matched++;
            break;
        }
        default:
            /* Unknown specifier, stop parsing */
            goto done;
        }
    }

done:
    va_end(ap);

#undef SS_ISSPACE
#undef SS_ISDIGIT
#undef SS_ISXDIGIT
#undef SS_ISODIGIT

    return matched;
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
    if (c == EOF || stream->has_unget) return EOF;
    stream->unget_char = c;
    stream->has_unget = 1;
    stream->eof = 0;
    return c;
}

int getc(FILE *stream) {
    return fgetc(stream);
}

int putc(int c, FILE *stream) {
    return fputc(c, stream);
}
