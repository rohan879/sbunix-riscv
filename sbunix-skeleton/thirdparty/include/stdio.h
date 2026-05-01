#pragma once
#include_next <stdio.h>

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

int sprintf(char *, const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int dprintf(int, const char *, ...);
int vprintf(const char *, va_list);
int vfprintf(FILE *, const char *, va_list);
int vsnprintf(char *, size_t, const char *, va_list);
FILE *fopen(const char *, const char *);
int fclose(FILE *);
size_t fread(void *, size_t, size_t, FILE *);
int fputc(int, FILE *);
int fputs(const char *, FILE *);
int fgetc(FILE *);
char *fgets(char *, int, FILE *);
int ferror(FILE *);
void clearerr(FILE *);
int puts(const char *s);
int putchar(int c);
#define putc(c, f) fputc(c, f)
#define getc(f)    fgetc(f)
size_t fwrite(const void *, size_t, size_t, FILE *);
int fseek(FILE *, long, int);
long ftell(FILE *);
int feof(FILE *);
int remove(const char *);
int rename(const char *oldpath, const char *newpath);
int sscanf(const char *, const char *, ...);
FILE *fdopen(int, const char *);
int fflush(FILE *);
int fileno(FILE *);
void rewind(FILE *);
int setvbuf(FILE *, char *, int, size_t);
void perror(const char *);
FILE *popen(const char *cmd, const char *mode);
int pclose(FILE *f);
