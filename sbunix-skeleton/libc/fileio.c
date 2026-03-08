/*
 * fileio.c — FILE* stream implementation for SBUnix libc.
 *
 * Provides fprintf, perror, fopen, fclose, fread, fwrite, etc.
 * All streams are unbuffered — operations go directly to read/write syscalls.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>

/* Static FILE objects for stdin, stdout, stderr */
static FILE __stdin  = { 0, 0, 0 };
static FILE __stdout = { 1, 0, 0 };
static FILE __stderr = { 2, 0, 0 };

FILE *stdin  = &__stdin;
FILE *stdout = &__stdout;
FILE *stderr = &__stderr;

/* Static pool of FILE objects for fopen */
#define FILE_POOL_SIZE 16
static FILE __file_pool[FILE_POOL_SIZE];
static int  __file_pool_used[FILE_POOL_SIZE];

int fprintf(FILE *stream, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vdprintf(stream->fd, fmt, ap);
    va_end(ap);
    return ret;
}

int vfprintf(FILE *stream, const char *fmt, va_list ap) {
    return vdprintf(stream->fd, fmt, ap);
}

void perror(const char *s) {
    if (s && *s)
        dprintf(2, "%s: %s\n", s, strerror(errno));
    else
        dprintf(2, "%s\n", strerror(errno));
}

int fputs(const char *s, FILE *stream) {
    size_t len = strlen(s);
    ssize_t r = write(stream->fd, s, len);
    if (r < 0) {
        stream->error = 1;
        return EOF;
    }
    return (int)r;
}

int fputc(int c, FILE *stream) {
    unsigned char ch = (unsigned char)c;
    ssize_t r = write(stream->fd, &ch, 1);
    if (r < 0) {
        stream->error = 1;
        return EOF;
    }
    return ch;
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
    ssize_t r = write(stream->fd, ptr, total);
    if (r < 0) {
        stream->error = 1;
        return 0;
    }
    return (size_t)r / size;
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
    (void)stream;
    return 0; /* unbuffered — nothing to flush */
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
    (void)stream; (void)buf; (void)mode; (void)size;
    return 0; /* unbuffered — always succeeds */
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
