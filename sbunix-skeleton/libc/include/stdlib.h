#ifndef _STDLIB_H
#define _STDLIB_H

#include <sys/types.h>

/* Process termination */
void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int  atexit(void (*func)(void));

/* Memory allocation */
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

/* Number conversion — integer */
int                atoi(const char *s);
long               atol(const char *s);
long               strtol(const char *s, char **endptr, int base);
unsigned long      strtoul(const char *s, char **endptr, int base);
long long          strtoll(const char *s, char **endptr, int base);
unsigned long long strtoull(const char *s, char **endptr, int base);

/* Number conversion — floating point */
double strtod(const char *s, char **endptr);
float  strtof(const char *s, char **endptr);
double atof(const char *s);

/* Integer to string */
char *itoa(int value, char *buf, int base);

/* Pseudo-random numbers */
int  rand(void);
void srand(unsigned int seed);

#define RAND_MAX 32767

/* Absolute value */
int  abs(int x);
long labs(long x);

/* Integer division */
typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;
div_t   div(int numer, int denom);
ldiv_t  ldiv(long numer, long denom);
lldiv_t lldiv(long long numer, long long denom);

/* Environment */
char *getenv(const char *name);
int    setenv(const char *name, const char *value, int overwrite);
int    unsetenv(const char *name);
int    putenv(char *string);

/* Sorting and searching */
void  qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

/* Shell command execution */
int system(const char *command);

/* Temporary files */
int mkstemp(char *tmpl);

/* Path resolution */
char *realpath(const char *path, char *resolved_path);

/* File removal */
int remove(const char *path);

/* Memory management */
void *sbrk(long increment);
int brk(void *addr);

/* Exit status codes */
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#endif /* _STDLIB_H */
