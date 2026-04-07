#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>

extern char **environ;
extern char *ptsname(int fd);

void _Exit(int status) {
  _exit(status);
}

int at_quick_exit(void (*func)(void)) {
  return atexit(func);
}

void quick_exit(int status) {
  _Exit(status);
}

void *aligned_alloc(size_t alignment, size_t size) {
  void *p;
  if (posix_memalign(&p, alignment, size) != 0)
    return (void *)0;
  return p;
}

/* BF: Rewrote posix_memalign to support alignments > 16 bytes.
 * The old version returned ENOMEM for alignment > 16, breaking tests
 * that request page-aligned or cache-line-aligned allocations.
 * For alignment <= 16, malloc already returns 16-byte aligned memory.
 * For larger alignments, we over-allocate and store the original
 * pointer + a magic sentinel before the aligned address so free()
 * can find the real allocation to release. */
#define ALIGNED_SENTINEL 0xA110CA7ED5E1F000ULL
int posix_memalign(void **memptr, size_t alignment, size_t size) {
  if (!memptr)
    return EINVAL;
  if (alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment < sizeof(void *))
    return EINVAL;

  if (alignment <= 16) {
    /* malloc already guarantees 16-byte alignment */
    *memptr = malloc(size);
    if (!*memptr)
      return ENOMEM;
    return 0;
  }

  /* Over-allocate: need room for the data, alignment padding, and
   * 2 pointer-sized values (sentinel + original pointer) stored
   * immediately before the aligned address. */
  size_t overhead = alignment + 2 * sizeof(uint64_t);
  void *raw = malloc(size + overhead);
  if (!raw)
    return ENOMEM;

  /* Find the first aligned address that leaves room for our 2 words */
  uintptr_t raw_addr = (uintptr_t)raw + 2 * sizeof(uint64_t);
  uintptr_t aligned = (raw_addr + alignment - 1) & ~(uintptr_t)(alignment - 1);

  /* Store sentinel magic and original pointer just before aligned addr */
  ((uint64_t *)aligned)[-2] = ALIGNED_SENTINEL;
  ((void **)aligned)[-1] = raw;

  *memptr = (void *)aligned;
  return 0;
}

void *reallocarray(void *ptr, size_t nmemb, size_t size) {
  if (size != 0 && nmemb > SIZE_MAX / size) {
    errno = ENOMEM;
    return (void *)0;
  }
  return realloc(ptr, nmemb * size);
}

long long atoll(const char *s) {
  return strtoll(s, (char **)0, 10);
}

long long llabs(long long x) {
  return x < 0 ? -x : x;
}

/* ENH: Delegate to strtod and cast, since on this RISC-V target
 * long double == double (both 64-bit IEEE 754). */
long double strtold(const char *s, char **endptr) {
  return (long double)strtod(s, endptr);
}

double drand48(void) {
  return 0.5;
}

double erand48(unsigned short xsubi[3]) {
  (void)xsubi;
  return 0.5;
}

long lrand48(void) {
  return (long)rand();
}

long nrand48(unsigned short xsubi[3]) {
  (void)xsubi;
  return (long)rand();
}

long mrand48(void) {
  return (long)((rand() << 1) ^ rand());
}

long jrand48(unsigned short xsubi[3]) {
  (void)xsubi;
  return mrand48();
}

void srand48(long seedval) {
  srand((unsigned int)seedval);
}

unsigned short *seed48(unsigned short seed16v[3]) {
  static unsigned short prev[3];
  prev[0] = 0;
  prev[1] = 0;
  prev[2] = 0;
  if (seed16v)
    srand((unsigned int)seed16v[0] ^ ((unsigned int)seed16v[1] << 8) ^ ((unsigned int)seed16v[2] << 16));
  return prev;
}

void lcong48(unsigned short param[7]) {
  if (param)
    srand((unsigned int)param[0]);
}

long random(void) {
  return (long)rand();
}

void srandom(unsigned int seed) {
  srand(seed);
}

char *initstate(unsigned int seed, char *state, size_t size) {
  (void)size;
  srandom(seed);
  return state;
}

char *setstate(char *state) {
  return state;
}

void qsort_r(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *, void *),
             void *arg) {
  size_t i, j;
  char *b = (char *)base;
  char tmp[256];

  if (!base || nmemb < 2 || size == 0 || !compar)
    return;

  for (i = 0; i < nmemb; i++) {
    for (j = i + 1; j < nmemb; j++) {
      if (compar(b + i * size, b + j * size, arg) > 0) {
        if (size <= sizeof(tmp)) {
          memcpy(tmp, b + i * size, size);
          memcpy(b + i * size, b + j * size, size);
          memcpy(b + j * size, tmp, size);
        } else {
          char *dyn = (char *)malloc(size);
          if (!dyn)
            return;
          memcpy(dyn, b + i * size, size);
          memcpy(b + i * size, b + j * size, size);
          memcpy(b + j * size, dyn, size);
          free(dyn);
        }
      }
    }
  }
}

int mkostemp(char *tmpl, int flags) {
  (void)flags;
  return mkstemp(tmpl);
}

char *mkdtemp(char *tmpl) {
  int len;
  int i;
  static const char alnum[] = "abcdefghijklmnopqrstuvwxyz0123456789";

  if (!tmpl)
    return (void *)0;
  len = (int)strlen(tmpl);
  if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0)
    return (void *)0;

  for (i = 0; i < 6; i++)
    tmpl[len - 6 + i] = alnum[(rand() + i) % (int)(sizeof(alnum) - 1)];

  if (mkdir(tmpl, 0700) < 0)
    return (void *)0;
  return tmpl;
}

int ptsname_r(int fd, char *buf, size_t buflen) {
  const char *p = ptsname(fd);
  size_t n;

  if (!p || !buf)
    return -1;
  n = strlen(p);
  if (n + 1 > buflen)
    return -1;
  memcpy(buf, p, n + 1);
  return 0;
}

int mblen(const char *s, size_t n) {
  (void)n;
  if (!s || !*s)
    return 0;
  return 1;
}

int mbtowc(wchar_t *pwc, const char *s, size_t n) {
  (void)n;
  if (!s)
    return 0;
  if (*s == '\0')
    return 0;
  if (pwc)
    *pwc = (unsigned char)*s;
  return 1;
}

size_t mbstowcs(wchar_t *pwcs, const char *s, size_t n) {
  size_t i = 0;
  if (!s)
    return 0;
  while (s[i] && i < n) {
    if (pwcs)
      pwcs[i] = (unsigned char)s[i];
    i++;
  }
  if (pwcs && i < n)
    pwcs[i] = 0;
  return i;
}

int wctomb(char *s, wchar_t wc) {
  if (!s)
    return 0;
  *s = (char)(unsigned int)wc;
  return 1;
}

size_t wcstombs(char *s, const wchar_t *pwcs, size_t n) {
  size_t i = 0;
  if (!pwcs)
    return 0;
  while (pwcs[i] && i < n) {
    if (s)
      s[i] = (char)(unsigned int)pwcs[i];
    i++;
  }
  if (s && i < n)
    s[i] = '\0';
  return i;
}

long a64l(const char *s) {
  return strtol(s, (char **)0, 64);
}

char *l64a(long value) {
  static char buf[32];
  static const char tbl[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  unsigned long v = (unsigned long)value;
  int i = 0;

  if (v == 0) {
    buf[0] = '\0';
    return buf;
  }

  while (v && i < (int)sizeof(buf) - 1) {
    buf[i++] = tbl[v & 0x3fUL];
    v >>= 6;
  }
  buf[i] = '\0';
  return buf;
}

char *secure_getenv(const char *name) {
  return getenv(name);
}

int getsubopt(char **optionp, char *const *tokens, char **valuep) {
  char *p;
  char *end;
  char *eq;
  int i;

  if (!optionp || !*optionp || !tokens || !valuep)
    return -1;

  p = *optionp;
  end = p;
  while (*end && *end != ',')
    end++;

  if (*end == ',') {
    *end = '\0';
    *optionp = end + 1;
  } else {
    *optionp = end;
  }

  eq = p;
  while (*eq && *eq != '=')
    eq++;
  if (*eq == '=') {
    *eq = '\0';
    *valuep = eq + 1;
  } else {
    *valuep = (char *)0;
  }

  for (i = 0; tokens[i]; i++) {
    if (strcmp(tokens[i], p) == 0)
      return i;
  }

  *valuep = p;
  return -1;
}
