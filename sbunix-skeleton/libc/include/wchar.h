#ifndef _WCHAR_H
#define _WCHAR_H

#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <locale.h>
#include <limits.h>

typedef int wint_t;

/* BF: Changed mbstate_t from plain int to struct to hold partial UTF-8
 * multibyte sequences across calls to mbrtowc/wcrtomb.  The old int
 * type could not track byte count, expected length, or pending data
 * needed for correct incremental decoding. */
typedef struct {
  unsigned char partial[4]; /* buffered bytes of incomplete sequence */
  int count;                /* number of bytes stored in partial[] */
  int expected;             /* total bytes expected for current sequence */
  unsigned int pending_surrogate; /* pending high surrogate for c16rtomb */
} mbstate_t;

#define WEOF ((wint_t)-1)

#ifndef WCHAR_MIN
#define WCHAR_MIN INT_MIN
#endif

#ifndef WCHAR_MAX
#define WCHAR_MAX INT_MAX
#endif

wchar_t *wcscpy(wchar_t *dest, const wchar_t *src);
wchar_t *wcsncpy(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wcscat(wchar_t *dest, const wchar_t *src);
wchar_t *wcsncat(wchar_t *dest, const wchar_t *src, size_t n);
int wcscmp(const wchar_t *s1, const wchar_t *s2);
int wcsncmp(const wchar_t *s1, const wchar_t *s2, size_t n);
size_t wcslen(const wchar_t *s);
size_t wcsnlen(const wchar_t *s, size_t maxlen);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle);

wchar_t *wcpcpy(wchar_t *dest, const wchar_t *src);
wchar_t *wcpncpy(wchar_t *dest, const wchar_t *src, size_t n);
size_t wcslcpy(wchar_t *dst, const wchar_t *src, size_t dsize);
size_t wcslcat(wchar_t *dst, const wchar_t *src, size_t dsize);

int wmemcmp(const wchar_t *s1, const wchar_t *s2, size_t n);
wchar_t *wmemcpy(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wmemmove(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);

wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **ptr);
size_t wcscspn(const wchar_t *s, const wchar_t *reject);
size_t wcsspn(const wchar_t *s, const wchar_t *accept);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept);

wchar_t *wcsdup(const wchar_t *s);

double wcstod(const wchar_t *nptr, wchar_t **endptr);
float wcstof(const wchar_t *nptr, wchar_t **endptr);
long double wcstold(const wchar_t *nptr, wchar_t **endptr);
long wcstol(const wchar_t *nptr, wchar_t **endptr, int base);
unsigned long wcstoul(const wchar_t *nptr, wchar_t **endptr, int base);
long long wcstoll(const wchar_t *nptr, wchar_t **endptr, int base);
unsigned long long wcstoull(const wchar_t *nptr, wchar_t **endptr, int base);

wint_t btowc(int c);
int wctob(wint_t c);
size_t mbrlen(const char *s, size_t n, mbstate_t *ps);
size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps);
int mbsinit(const mbstate_t *ps);
size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps);
size_t mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t len,
                  mbstate_t *ps);
size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *ps);
size_t wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len,
                  mbstate_t *ps);

int fwide(FILE *stream, int mode);
wint_t fgetwc(FILE *stream);
wchar_t *fgetws(wchar_t *ws, int n, FILE *stream);
wint_t fputwc(wchar_t wc, FILE *stream);
int fputws(const wchar_t *ws, FILE *stream);
wint_t getwc(FILE *stream);
wint_t getwchar(void);
wint_t putwc(wchar_t wc, FILE *stream);
wint_t putwchar(wchar_t wc);
wint_t ungetwc(wint_t wc, FILE *stream);

int fwprintf(FILE *stream, const wchar_t *format, ...);
int wprintf(const wchar_t *format, ...);
int swprintf(wchar_t *ws, size_t n, const wchar_t *format, ...);
int vfwprintf(FILE *stream, const wchar_t *format, va_list ap);
int vwprintf(const wchar_t *format, va_list ap);
int vswprintf(wchar_t *ws, size_t n, const wchar_t *format, va_list ap);

int fwscanf(FILE *stream, const wchar_t *format, ...);
int wscanf(const wchar_t *format, ...);
int swscanf(const wchar_t *ws, const wchar_t *format, ...);
int vfwscanf(FILE *stream, const wchar_t *format, va_list ap);
int vwscanf(const wchar_t *format, va_list ap);
int vswscanf(const wchar_t *ws, const wchar_t *format, va_list ap);

int wcswidth(const wchar_t *s, size_t n);
int wcwidth(wchar_t wc);

size_t wcsxfrm(wchar_t *dest, const wchar_t *src, size_t n);
size_t wcsxfrm_l(wchar_t *dest, const wchar_t *src, size_t n, locale_t l);
int wcscoll(const wchar_t *s1, const wchar_t *s2);
int wcscoll_l(const wchar_t *s1, const wchar_t *s2, locale_t l);
int wcscasecmp(const wchar_t *s1, const wchar_t *s2);
int wcscasecmp_l(const wchar_t *s1, const wchar_t *s2, locale_t l);
int wcsncasecmp(const wchar_t *s1, const wchar_t *s2, size_t n);
int wcsncasecmp_l(const wchar_t *s1, const wchar_t *s2, size_t n, locale_t l);

size_t wcsftime(wchar_t *wcs, size_t maxsize, const wchar_t *format,
                const struct tm *timeptr);

FILE *open_wmemstream(wchar_t **ptr, size_t *sizeloc);

#endif
