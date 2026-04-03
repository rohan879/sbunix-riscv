#include <wchar.h>
#include <wctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

extern int fgetc(FILE *stream);
extern int fputc(int c, FILE *stream);
extern int ungetc(int c, FILE *stream);
extern int vfprintf(FILE *stream, const char *fmt, va_list ap);
extern int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
extern int vfscanf(FILE *stream, const char *fmt, va_list ap);
extern int vsscanf(const char *str, const char *fmt, va_list ap);
extern FILE *stdin;
extern FILE *stdout;
#ifndef EOF
#define EOF (-1)
#endif

static size_t wlen(const wchar_t *s) {
  size_t n = 0;
  if (!s)
    return 0;
  while (s[n])
    n++;
  return n;
}

static int wcmpn(const wchar_t *a, const wchar_t *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (a[i] != b[i])
      return (a[i] < b[i]) ? -1 : 1;
    if (!a[i])
      return 0;
  }
  return 0;
}

static int tolower_ascii(int c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A' + 'a';
  return c;
}

wchar_t *wcscpy(wchar_t *dest, const wchar_t *src) {
  wchar_t *d = dest;
  while ((*d++ = *src++) != 0)
    ;
  return dest;
}

wchar_t *wcsncpy(wchar_t *dest, const wchar_t *src, size_t n) {
  size_t i = 0;
  while (i < n && src[i]) {
    dest[i] = src[i];
    i++;
  }
  while (i < n)
    dest[i++] = 0;
  return dest;
}

wchar_t *wcscat(wchar_t *dest, const wchar_t *src) {
  return wcscpy(dest + wlen(dest), src);
}

wchar_t *wcsncat(wchar_t *dest, const wchar_t *src, size_t n) {
  size_t d = wlen(dest);
  size_t i = 0;
  while (i < n && src[i]) {
    dest[d + i] = src[i];
    i++;
  }
  dest[d + i] = 0;
  return dest;
}

int wcscmp(const wchar_t *s1, const wchar_t *s2) {
  while (*s1 && *s1 == *s2) {
    s1++;
    s2++;
  }
  return (*s1 < *s2) ? -1 : (*s1 > *s2);
}

int wcsncmp(const wchar_t *s1, const wchar_t *s2, size_t n) {
  return wcmpn(s1, s2, n);
}

size_t wcslen(const wchar_t *s) {
  return wlen(s);
}

size_t wcsnlen(const wchar_t *s, size_t maxlen) {
  size_t i = 0;
  while (i < maxlen && s[i])
    i++;
  return i;
}

wchar_t *wcschr(const wchar_t *s, wchar_t c) {
  while (*s) {
    if (*s == c)
      return (wchar_t *)s;
    s++;
  }
  return c == 0 ? (wchar_t *)s : (void *)0;
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c) {
  const wchar_t *last = (void *)0;
  while (*s) {
    if (*s == c)
      last = s;
    s++;
  }
  if (c == 0)
    return (wchar_t *)s;
  return (wchar_t *)last;
}

wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle) {
  size_t nlen = wlen(needle);
  if (nlen == 0)
    return (wchar_t *)haystack;
  while (*haystack) {
    if (wcmpn(haystack, needle, nlen) == 0)
      return (wchar_t *)haystack;
    haystack++;
  }
  return (void *)0;
}

wchar_t *wcpcpy(wchar_t *dest, const wchar_t *src) {
  while ((*dest = *src) != 0) {
    dest++;
    src++;
  }
  return dest;
}

wchar_t *wcpncpy(wchar_t *dest, const wchar_t *src, size_t n) {
  size_t i = 0;
  while (i < n && src[i]) {
    dest[i] = src[i];
    i++;
  }
  while (i < n)
    dest[i++] = 0;
  return dest + (i < n ? i : n);
}

size_t wcslcpy(wchar_t *dst, const wchar_t *src, size_t dsize) {
  size_t sl = wlen(src);
  if (dsize) {
    size_t n = (sl >= dsize) ? dsize - 1 : sl;
    for (size_t i = 0; i < n; i++)
      dst[i] = src[i];
    dst[n] = 0;
  }
  return sl;
}

size_t wcslcat(wchar_t *dst, const wchar_t *src, size_t dsize) {
  size_t dl = wlen(dst);
  size_t sl = wlen(src);
  if (dl >= dsize)
    return dsize + sl;
  size_t room = dsize - dl - 1;
  size_t n = (sl > room) ? room : sl;
  for (size_t i = 0; i < n; i++)
    dst[dl + i] = src[i];
  dst[dl + n] = 0;
  return dl + sl;
}

int wmemcmp(const wchar_t *s1, const wchar_t *s2, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s1[i] != s2[i])
      return (s1[i] < s2[i]) ? -1 : 1;
  }
  return 0;
}

wchar_t *wmemcpy(wchar_t *dest, const wchar_t *src, size_t n) {
  for (size_t i = 0; i < n; i++)
    dest[i] = src[i];
  return dest;
}

wchar_t *wmemmove(wchar_t *dest, const wchar_t *src, size_t n) {
  if (dest < src) {
    for (size_t i = 0; i < n; i++)
      dest[i] = src[i];
  } else if (dest > src) {
    for (size_t i = n; i > 0; i--)
      dest[i - 1] = src[i - 1];
  }
  return dest;
}

wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n) {
  for (size_t i = 0; i < n; i++)
    s[i] = c;
  return s;
}

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s[i] == c)
      return (wchar_t *)(s + i);
  }
  return (void *)0;
}

wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **ptr) {
  wchar_t *tok;
  if (!s)
    s = *ptr;
  while (*s && wcschr(delim, *s))
    s++;
  if (!*s)
    return (void *)0;
  tok = s;
  while (*s && !wcschr(delim, *s))
    s++;
  if (*s) {
    *s = 0;
    s++;
  }
  *ptr = s;
  return tok;
}

size_t wcscspn(const wchar_t *s, const wchar_t *reject) {
  size_t i = 0;
  while (s[i] && !wcschr(reject, s[i]))
    i++;
  return i;
}

size_t wcsspn(const wchar_t *s, const wchar_t *accept) {
  size_t i = 0;
  while (s[i] && wcschr(accept, s[i]))
    i++;
  return i;
}

wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept) {
  while (*s) {
    if (wcschr(accept, *s))
      return (wchar_t *)s;
    s++;
  }
  return (void *)0;
}

wchar_t *wcsdup(const wchar_t *s) {
  size_t n = wlen(s) + 1;
  wchar_t *d = (wchar_t *)malloc(n * sizeof(wchar_t));
  if (!d)
    return (void *)0;
  for (size_t i = 0; i < n; i++)
    d[i] = s[i];
  return d;
}

double wcstod(const wchar_t *nptr, wchar_t **endptr) {
  size_t n = wlen(nptr);
  char *tmp = (char *)malloc(n + 1);
  char *end = (void *)0;
  double v;
  if (!tmp)
    return 0.0;
  for (size_t i = 0; i <= n; i++)
    tmp[i] = (char)(unsigned int)nptr[i];
  v = strtod(tmp, &end);
  if (endptr)
    *endptr = (wchar_t *)(nptr + (end - tmp));
  free(tmp);
  return v;
}

float wcstof(const wchar_t *nptr, wchar_t **endptr) {
  size_t n = wlen(nptr);
  char *tmp = (char *)malloc(n + 1);
  char *end = (void *)0;
  float v;
  if (!tmp)
    return 0.0f;
  for (size_t i = 0; i <= n; i++)
    tmp[i] = (char)(unsigned int)nptr[i];
  v = strtof(tmp, &end);
  if (endptr)
    *endptr = (wchar_t *)(nptr + (end - tmp));
  free(tmp);
  return v;
}

long double wcstold(const wchar_t *nptr, wchar_t **endptr) {
  size_t n = wlen(nptr);
  char *tmp = (char *)malloc(n + 1);
  char *end = (void *)0;
  long double v;
  if (!tmp)
    return 0.0L;
  for (size_t i = 0; i <= n; i++)
    tmp[i] = (char)(unsigned int)nptr[i];
  v = strtold(tmp, &end);
  if (endptr)
    *endptr = (wchar_t *)(nptr + (end - tmp));
  free(tmp);
  return v;
}

long wcstol(const wchar_t *nptr, wchar_t **endptr, int base) {
  size_t n = wlen(nptr);
  char *tmp = (char *)malloc(n + 1);
  char *end = (void *)0;
  long v;
  if (!tmp)
    return 0;
  for (size_t i = 0; i <= n; i++)
    tmp[i] = (char)(unsigned int)nptr[i];
  v = strtol(tmp, &end, base);
  if (endptr)
    *endptr = (wchar_t *)(nptr + (end - tmp));
  free(tmp);
  return v;
}

unsigned long wcstoul(const wchar_t *nptr, wchar_t **endptr, int base) {
  size_t n = wlen(nptr);
  char *tmp = (char *)malloc(n + 1);
  char *end = (void *)0;
  unsigned long v;
  if (!tmp)
    return 0;
  for (size_t i = 0; i <= n; i++)
    tmp[i] = (char)(unsigned int)nptr[i];
  v = strtoul(tmp, &end, base);
  if (endptr)
    *endptr = (wchar_t *)(nptr + (end - tmp));
  free(tmp);
  return v;
}

long long wcstoll(const wchar_t *nptr, wchar_t **endptr, int base) {
  return (long long)wcstol(nptr, endptr, base);
}

unsigned long long wcstoull(const wchar_t *nptr, wchar_t **endptr, int base) {
  return (unsigned long long)wcstoul(nptr, endptr, base);
}

wint_t btowc(int c) { return (wint_t)(unsigned char)c; }
int wctob(wint_t c) { return (int)(unsigned int)c; }

size_t mbrlen(const char *s, size_t n, mbstate_t *ps) {
  (void)n;
  (void)ps;
  if (!s || !*s)
    return 0;
  return 1;
}

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps) {
  (void)n;
  (void)ps;
  if (!s || !*s)
    return 0;
  if (pwc)
    *pwc = (unsigned char)*s;
  return 1;
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps) {
  (void)ps;
  if (!s)
    return 1;
  *s = (char)(unsigned int)wc;
  return 1;
}

int mbsinit(const mbstate_t *ps) {
  (void)ps;
  return 1;
}

size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps) {
  (void)ps;
  size_t i = 0;
  const char *s = *src;
  while (s[i] && i < len) {
    if (dst)
      dst[i] = (unsigned char)s[i];
    i++;
  }
  if (dst && i < len)
    dst[i] = 0;
  *src = s + i;
  return i;
}

size_t mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t len,
                  mbstate_t *ps) {
  (void)ps;
  size_t i = 0;
  const char *s = *src;
  while (i < nms && s[i] && i < len) {
    if (dst)
      dst[i] = (unsigned char)s[i];
    i++;
  }
  if (dst && i < len)
    dst[i] = 0;
  *src = s + i;
  return i;
}

size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *ps) {
  (void)ps;
  size_t i = 0;
  const wchar_t *s = *src;
  while (s[i] && i < len) {
    if (dst)
      dst[i] = (char)(unsigned int)s[i];
    i++;
  }
  if (dst && i < len)
    dst[i] = '\0';
  *src = s + i;
  return i;
}

size_t wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len,
                  mbstate_t *ps) {
  (void)ps;
  size_t i = 0;
  const wchar_t *s = *src;
  while (i < nwc && s[i] && i < len) {
    if (dst)
      dst[i] = (char)(unsigned int)s[i];
    i++;
  }
  if (dst && i < len)
    dst[i] = '\0';
  *src = s + i;
  return i;
}

int fwide(FILE *stream, int mode) {
  (void)stream;
  (void)mode;
  return 0;
}

wint_t fgetwc(FILE *stream) { return (wint_t)fgetc(stream); }

wchar_t *fgetws(wchar_t *ws, int n, FILE *stream) {
  for (int i = 0; i < n - 1; i++) {
    int c = fgetc(stream);
    if (c == EOF) {
      if (i == 0)
        return (void *)0;
      ws[i] = 0;
      return ws;
    }
    ws[i] = (wchar_t)c;
    if (c == '\n') {
      ws[i + 1] = 0;
      return ws;
    }
  }
  ws[n - 1] = 0;
  return ws;
}

wint_t fputwc(wchar_t wc, FILE *stream) { return (wint_t)fputc((int)wc, stream); }

int fputws(const wchar_t *ws, FILE *stream) {
  int n = 0;
  while (*ws) {
    if (fputc((int)*ws++, stream) == EOF)
      return EOF;
    n++;
  }
  return n;
}

wint_t getwc(FILE *stream) { return fgetwc(stream); }
wint_t getwchar(void) { return fgetwc(stdin); }
wint_t putwc(wchar_t wc, FILE *stream) { return fputwc(wc, stream); }
wint_t putwchar(wchar_t wc) { return fputwc(wc, stdout); }
wint_t ungetwc(wint_t wc, FILE *stream) { return (wint_t)ungetc((int)wc, stream); }

static size_t wtoa(const wchar_t *ws, char *buf, size_t cap) {
  size_t i = 0;
  if (!cap)
    return 0;
  while (ws[i] && i + 1 < cap) {
    buf[i] = (char)(unsigned int)ws[i];
    i++;
  }
  buf[i] = '\0';
  return i;
}

int vfwprintf(FILE *stream, const wchar_t *format, va_list ap) {
  char fmt[512];
  wtoa(format, fmt, sizeof(fmt));
  return vfprintf(stream, fmt, ap);
}

int vwprintf(const wchar_t *format, va_list ap) {
  return vfwprintf(stdout, format, ap);
}

int fwprintf(FILE *stream, const wchar_t *format, ...) {
  va_list ap;
  int r;
  va_start(ap, format);
  r = vfwprintf(stream, format, ap);
  va_end(ap);
  return r;
}

int wprintf(const wchar_t *format, ...) {
  va_list ap;
  int r;
  va_start(ap, format);
  r = vfwprintf(stdout, format, ap);
  va_end(ap);
  return r;
}

int vswprintf(wchar_t *ws, size_t n, const wchar_t *format, va_list ap) {
  char fmt[256];
  char out[1024];
  int r;
  wtoa(format, fmt, sizeof(fmt));
  r = vsnprintf(out, sizeof(out), fmt, ap);
  if (r < 0)
    return r;
  size_t m = (size_t)r;
  if (m >= n)
    m = n ? n - 1 : 0;
  for (size_t i = 0; i < m; i++)
    ws[i] = (unsigned char)out[i];
  if (n)
    ws[m] = 0;
  return r;
}

int swprintf(wchar_t *ws, size_t n, const wchar_t *format, ...) {
  va_list ap;
  int r;
  va_start(ap, format);
  r = vswprintf(ws, n, format, ap);
  va_end(ap);
  return r;
}

int vfwscanf(FILE *stream, const wchar_t *format, va_list ap) {
  char fmt[256];
  wtoa(format, fmt, sizeof(fmt));
  return vfscanf(stream, fmt, ap);
}

int vwscanf(const wchar_t *format, va_list ap) {
  return vfwscanf(stdin, format, ap);
}

int fwscanf(FILE *stream, const wchar_t *format, ...) {
  va_list ap;
  int r;
  va_start(ap, format);
  r = vfwscanf(stream, format, ap);
  va_end(ap);
  return r;
}

int wscanf(const wchar_t *format, ...) {
  va_list ap;
  int r;
  va_start(ap, format);
  r = vfwscanf(stdin, format, ap);
  va_end(ap);
  return r;
}

int vswscanf(const wchar_t *ws, const wchar_t *format, va_list ap) {
  char s[1024];
  char f[256];
  wtoa(ws, s, sizeof(s));
  wtoa(format, f, sizeof(f));
  return vsscanf(s, f, ap);
}

int swscanf(const wchar_t *ws, const wchar_t *format, ...) {
  va_list ap;
  int r;
  va_start(ap, format);
  r = vswscanf(ws, format, ap);
  va_end(ap);
  return r;
}

int wcswidth(const wchar_t *s, size_t n) {
  int w = 0;
  size_t i = 0;
  while (i < n && s[i]) {
    w++;
    i++;
  }
  return w;
}

int wcwidth(wchar_t wc) {
  (void)wc;
  return 1;
}

size_t wcsxfrm(wchar_t *dest, const wchar_t *src, size_t n) {
  size_t sl = wlen(src);
  if (n) {
    size_t m = (sl >= n) ? n - 1 : sl;
    for (size_t i = 0; i < m; i++)
      dest[i] = src[i];
    dest[m] = 0;
  }
  return sl;
}

size_t wcsxfrm_l(wchar_t *dest, const wchar_t *src, size_t n, locale_t l) {
  (void)l;
  return wcsxfrm(dest, src, n);
}

int wcscoll(const wchar_t *s1, const wchar_t *s2) { return wcscmp(s1, s2); }

int wcscoll_l(const wchar_t *s1, const wchar_t *s2, locale_t l) {
  (void)l;
  return wcscmp(s1, s2);
}

int wcscasecmp(const wchar_t *s1, const wchar_t *s2) {
  while (*s1 && *s2) {
    int c1 = tolower_ascii(*s1);
    int c2 = tolower_ascii(*s2);
    if (c1 != c2)
      return c1 < c2 ? -1 : 1;
    s1++;
    s2++;
  }
  return (*s1 < *s2) ? -1 : (*s1 > *s2);
}

int wcscasecmp_l(const wchar_t *s1, const wchar_t *s2, locale_t l) {
  (void)l;
  return wcscasecmp(s1, s2);
}

int wcsncasecmp(const wchar_t *s1, const wchar_t *s2, size_t n) {
  for (size_t i = 0; i < n; i++) {
    int c1 = tolower_ascii(s1[i]);
    int c2 = tolower_ascii(s2[i]);
    if (c1 != c2)
      return c1 < c2 ? -1 : 1;
    if (!s1[i])
      return 0;
  }
  return 0;
}

int wcsncasecmp_l(const wchar_t *s1, const wchar_t *s2, size_t n, locale_t l) {
  (void)l;
  return wcsncasecmp(s1, s2, n);
}

size_t wcsftime(wchar_t *wcs, size_t maxsize, const wchar_t *format,
                const struct tm *timeptr) {
  char fmt[128];
  char out[256];
  size_t n;
  wtoa(format, fmt, sizeof(fmt));
  n = strftime(out, sizeof(out), fmt, timeptr);
  if (maxsize == 0 || n >= maxsize)
    return 0;
  for (size_t i = 0; i <= n; i++)
    wcs[i] = (unsigned char)out[i];
  return n;
}

int iswalnum(wint_t wc) { return (wc >= 'a' && wc <= 'z') || (wc >= 'A' && wc <= 'Z') || (wc >= '0' && wc <= '9'); }
int iswalpha(wint_t wc) { return (wc >= 'a' && wc <= 'z') || (wc >= 'A' && wc <= 'Z'); }
int iswblank(wint_t wc) { return wc == ' ' || wc == '\t'; }
int iswcntrl(wint_t wc) { return (wc >= 0 && wc < 32) || wc == 127; }
int iswdigit(wint_t wc) { return wc >= '0' && wc <= '9'; }
int iswgraph(wint_t wc) { return wc > 32 && wc <= 126; }
int iswlower(wint_t wc) { return wc >= 'a' && wc <= 'z'; }
int iswprint(wint_t wc) { return wc >= 32 && wc <= 126; }
int iswpunct(wint_t wc) { return iswgraph(wc) && !iswalnum(wc); }
int iswspace(wint_t wc) { return wc == ' ' || wc == '\t' || wc == '\n' || wc == '\r' || wc == '\f' || wc == '\v'; }
int iswupper(wint_t wc) { return wc >= 'A' && wc <= 'Z'; }
int iswxdigit(wint_t wc) { return iswdigit(wc) || (wc >= 'a' && wc <= 'f') || (wc >= 'A' && wc <= 'F'); }

int iswalnum_l(wint_t wc, locale_t l) { (void)l; return iswalnum(wc); }
int iswalpha_l(wint_t wc, locale_t l) { (void)l; return iswalpha(wc); }
int iswblank_l(wint_t wc, locale_t l) { (void)l; return iswblank(wc); }
int iswcntrl_l(wint_t wc, locale_t l) { (void)l; return iswcntrl(wc); }
int iswdigit_l(wint_t wc, locale_t l) { (void)l; return iswdigit(wc); }
int iswgraph_l(wint_t wc, locale_t l) { (void)l; return iswgraph(wc); }
int iswlower_l(wint_t wc, locale_t l) { (void)l; return iswlower(wc); }
int iswprint_l(wint_t wc, locale_t l) { (void)l; return iswprint(wc); }
int iswpunct_l(wint_t wc, locale_t l) { (void)l; return iswpunct(wc); }
int iswspace_l(wint_t wc, locale_t l) { (void)l; return iswspace(wc); }
int iswupper_l(wint_t wc, locale_t l) { (void)l; return iswupper(wc); }
int iswxdigit_l(wint_t wc, locale_t l) { (void)l; return iswxdigit(wc); }

wint_t towlower(wint_t wc) {
  if (wc >= 'A' && wc <= 'Z')
    return wc - 'A' + 'a';
  return wc;
}

wint_t towupper(wint_t wc) {
  if (wc >= 'a' && wc <= 'z')
    return wc - 'a' + 'A';
  return wc;
}

wint_t towlower_l(wint_t wc, locale_t l) { (void)l; return towlower(wc); }
wint_t towupper_l(wint_t wc, locale_t l) { (void)l; return towupper(wc); }

wctype_t wctype(const char *name) {
  if (!name)
    return 0;
  if (strcmp(name, "alnum") == 0)
    return 1;
  if (strcmp(name, "alpha") == 0)
    return 2;
  if (strcmp(name, "blank") == 0)
    return 3;
  if (strcmp(name, "cntrl") == 0)
    return 4;
  if (strcmp(name, "digit") == 0)
    return 5;
  if (strcmp(name, "graph") == 0)
    return 6;
  if (strcmp(name, "lower") == 0)
    return 7;
  if (strcmp(name, "print") == 0)
    return 8;
  if (strcmp(name, "punct") == 0)
    return 9;
  if (strcmp(name, "space") == 0)
    return 10;
  if (strcmp(name, "upper") == 0)
    return 11;
  if (strcmp(name, "xdigit") == 0)
    return 12;
  return 0;
}

wctype_t wctype_l(const char *name, locale_t l) { (void)l; return wctype(name); }

int iswctype(wint_t wc, wctype_t desc) {
  switch ((unsigned long)desc) {
    case 1: return iswalnum(wc);
    case 2: return iswalpha(wc);
    case 3: return iswblank(wc);
    case 4: return iswcntrl(wc);
    case 5: return iswdigit(wc);
    case 6: return iswgraph(wc);
    case 7: return iswlower(wc);
    case 8: return iswprint(wc);
    case 9: return iswpunct(wc);
    case 10: return iswspace(wc);
    case 11: return iswupper(wc);
    case 12: return iswxdigit(wc);
    default: return 0;
  }
}

int iswctype_l(wint_t wc, wctype_t desc, locale_t l) {
  (void)l;
  return iswctype(wc, desc);
}

wctrans_t wctrans(const char *name) {
  if (!name)
    return (wctrans_t)0;
  if (strcmp(name, "tolower") == 0)
    return (wctrans_t)1;
  if (strcmp(name, "toupper") == 0)
    return (wctrans_t)2;
  return (wctrans_t)0;
}

wctrans_t wctrans_l(const char *name, locale_t l) { (void)l; return wctrans(name); }

wint_t towctrans(wint_t wc, wctrans_t desc) {
  if ((unsigned long)desc == 1)
    return towlower(wc);
  if ((unsigned long)desc == 2)
    return towupper(wc);
  return wc;
}

wint_t towctrans_l(wint_t wc, wctrans_t desc, locale_t l) {
  (void)l;
  return towctrans(wc, desc);
}
