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

/* Number conversion */
int           atoi(const char *s);
long          atol(const char *s);
long          strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);
long long          strtoll(const char *s, char **endptr, int base);
unsigned long long strtoull(const char *s, char **endptr, int base);

/* Integer to string */
char *itoa(int value, char *buf, int base);

/* Pseudo-random numbers */
int  rand(void);
void srand(unsigned int seed);

#define RAND_MAX 32767

/* Absolute value */
int  abs(int x);
long labs(long x);

/* Environment */
char *getenv(const char *name);
int    setenv(const char *name, const char *value, int overwrite);
int    unsetenv(const char *name);
int    putenv(char *string);

/* Path resolution */
char *realpath(const char *path, char *resolved_path);

/* Command execution */
int system(const char *command);

/* Sorting and searching */
void  qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

/* Exit status codes */
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#endif /* _STDLIB_H */
