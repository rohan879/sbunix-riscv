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
    (void)str; (void)fmt;
    return 0; /* stub */
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
    (void)stream;
    /* Not supported in unbuffered mode */
    return c;
}

int getc(FILE *stream) {
    return fgetc(stream);
}

int putc(int c, FILE *stream) {
    return fputc(c, stream);
}
