#ifndef _STDIO_H
#define _STDIO_H

#include <sys/types.h>
#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)
#define BUFSIZ 1024
#define FILENAME_MAX 256
#define FOPEN_MAX 16
#define L_ctermid 32
#define L_tmpnam  32
#define TMP_MAX   238328

typedef long fpos_t;

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
    char rbuf[FILE_BUF_SIZE];    /* read buffer -- enables buffered reads
                                  * instead of per-character read() syscalls */
    int  rpos;                   /* current read position in buffer */
    int  rlen;                   /* valid bytes in read buffer */
    int  unget_char;             /* pushed-back character */
    int  has_unget;              /* 1 if unget_char is valid */
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define stdin  stdin
#define stdout stdout
#define stderr stderr

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
int fseeko(FILE *stream, off_t offset, int whence);
off_t ftello(FILE *stream);
void rewind(FILE *stream);
int fgetpos(FILE *stream, fpos_t *pos);
int fsetpos(FILE *stream, const fpos_t *pos);
int sscanf(const char *str, const char *fmt, ...);
int fscanf(FILE *stream, const char *fmt, ...);
int vfscanf(FILE *stream, const char *fmt, va_list ap);
int scanf(const char *fmt, ...);
int vscanf(const char *fmt, va_list ap);
int vsscanf(const char *str, const char *fmt, va_list ap);
void perror(const char *s);
int setvbuf(FILE *stream, char *buf, int mode, size_t size);
void setbuf(FILE *stream, char *buf);
int ungetc(int c, FILE *stream);
int getc(FILE *stream);
int putc(int c, FILE *stream);
FILE *fdopen(int fd, const char *mode);
FILE *tmpfile(void);
char *tmpnam(char *s);
char *ctermid(char *s);
FILE *popen(const char *command, const char *type);
int   pclose(FILE *stream);
FILE *fmemopen(void *buf, size_t size, const char *mode);
FILE *open_memstream(char **ptr, size_t *sizeloc);

int asprintf(char **strp, const char *fmt, ...);
int vasprintf(char **strp, const char *fmt, va_list ap);

void flockfile(FILE *stream);
int ftrylockfile(FILE *stream);
void funlockfile(FILE *stream);
int getc_unlocked(FILE *stream);
int getchar_unlocked(void);
int putc_unlocked(int c, FILE *stream);
int putchar_unlocked(int c);

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
int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath);
int remove(const char *path);

#endif /* _STDIO_H */
