#ifndef _STDLIB_H
#define _STDLIB_H

#include <sys/types.h>
#include <stddef.h>
#include <fcntl.h>
#include <sys/wait.h>

/* Process termination */
void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int  atexit(void (*func)(void));
void _Exit(int status) __attribute__((noreturn));
int  at_quick_exit(void (*func)(void));
void quick_exit(int status) __attribute__((noreturn));

/* Memory allocation */
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);
void *aligned_alloc(size_t alignment, size_t size);
int   posix_memalign(void **memptr, size_t alignment, size_t size);
void *reallocarray(void *ptr, size_t nmemb, size_t size);

/* Number conversion — integer */
int                atoi(const char *s);
long               atol(const char *s);
long long          atoll(const char *s);
long               strtol(const char *s, char **endptr, int base);
unsigned long      strtoul(const char *s, char **endptr, int base);
long long          strtoll(const char *s, char **endptr, int base);
unsigned long long strtoull(const char *s, char **endptr, int base);
long double        strtold(const char *s, char **endptr);

long long          llabs(long long x);

/* Number conversion — floating point */
double strtod(const char *s, char **endptr);
float  strtof(const char *s, char **endptr);
double atof(const char *s);

/* Integer to string */
char *itoa(int value, char *buf, int base);

/* Pseudo-random numbers */
int  rand(void);
void srand(unsigned int seed);

double drand48(void);
double erand48(unsigned short xsubi[3]);
long lrand48(void);
long nrand48(unsigned short xsubi[3]);
long mrand48(void);
long jrand48(unsigned short xsubi[3]);
void srand48(long seedval);
unsigned short *seed48(unsigned short seed16v[3]);
void lcong48(unsigned short param[7]);

long random(void);
void srandom(unsigned int seed);
char *initstate(unsigned int seed, char *state, size_t size);
char *setstate(char *state);

#define RAND_MAX 32767
#define MB_CUR_MAX 4

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
void  qsort_r(void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *, void *),
              void *arg);
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

/* Shell command execution */
int system(const char *command);

/* Temporary files */
int mkstemp(char *tmpl);
int mkostemp(char *tmpl, int flags);
char *mkdtemp(char *tmpl);
int posix_openpt(int oflag);
int grantpt(int fd);
int unlockpt(int fd);
char *ptsname(int fd);
int ptsname_r(int fd, char *buf, size_t buflen);
char *tmpnam(char *s);

int getsubopt(char **optionp, char *const *tokens, char **valuep);

int mblen(const char *s, size_t n);
int mbtowc(wchar_t *pwc, const char *s, size_t n);
size_t mbstowcs(wchar_t *pwcs, const char *s, size_t n);
int wctomb(char *s, wchar_t wc);
size_t wcstombs(char *s, const wchar_t *pwcs, size_t n);

long a64l(const char *s);
char *l64a(long value);

void setkey(const char *key);

char *secure_getenv(const char *name);

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
