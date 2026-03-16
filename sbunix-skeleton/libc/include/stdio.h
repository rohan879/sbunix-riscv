#ifndef _STDIO_H
#define _STDIO_H

#include <sys/types.h>
#include <stdarg.h>

#define EOF (-1)
#define BUFSIZ 1024
#define FILENAME_MAX 256
#define FOPEN_MAX 16

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define FILE_BUF_SIZE 256

/* FILE stream type */
typedef struct _FILE {
    int fd;
    int eof;
    int error;
    int buf_mode;                 /* _IONBF, _IOLBF, or _IOFBF */
    char wbuf[FILE_BUF_SIZE];    /* write buffer */
    int  wpos;                   /* current position in write buffer */
    int  unget_char;             /* pushed-back character */
    int  has_unget;              /* 1 if unget_char is valid */
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

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

/* FILE stream I/O */
int fprintf(FILE *stream, const char *fmt, ...);
int vfprintf(FILE *stream, const char *fmt, va_list ap);
int fputs(const char *s, FILE *stream);
int fputc(int c, FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int fgetc(FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
int fflush(FILE *stream);
int fileno(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);
int sscanf(const char *str, const char *fmt, ...);
void perror(const char *s);
int setvbuf(FILE *stream, char *buf, int mode, size_t size);
void setbuf(FILE *stream, char *buf);
int ungetc(int c, FILE *stream);
int getc(FILE *stream);
int putc(int c, FILE *stream);
FILE *fdopen(int fd, const char *mode);
FILE *tmpfile(void);
FILE *popen(const char *command, const char *type);
int   pclose(FILE *stream);

/* Reopen */
FILE *freopen(const char *path, const char *mode, FILE *stream);

/* Line reading */
ssize_t getline(char **lineptr, size_t *n, FILE *stream);
ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream);

/* Basic I/O */
int puts(const char *s);
int putchar(int c);
int getchar(void);

/* rename (also in unistd.h) */
int rename(const char *oldpath, const char *newpath);

#endif /* _STDIO_H */
