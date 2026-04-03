#include <inttypes.h>
#include <stdlib.h>

static size_t wstrlen_local(const wchar_t *s) {
  size_t n = 0;
  if (!s)
    return 0;
  while (s[n])
    n++;
  return n;
}

static void wtoa_local(const wchar_t *in, char *out, size_t cap) {
  size_t i = 0;
  if (!out || cap == 0)
    return;
  if (!in) {
    out[0] = '\0';
    return;
  }
  while (in[i] && i + 1 < cap) {
    out[i] = (char)(unsigned int)in[i];
    i++;
  }
  out[i] = '\0';
}

intmax_t imaxabs(intmax_t j) {
  return j < 0 ? -j : j;
}

imaxdiv_t imaxdiv(intmax_t numer, intmax_t denom) {
  imaxdiv_t v;
  v.quot = numer / denom;
  v.rem = numer % denom;
  return v;
}

intmax_t strtoimax(const char *nptr, char **endptr, int base) {
  return (intmax_t)strtoll(nptr, endptr, base);
}

uintmax_t strtoumax(const char *nptr, char **endptr, int base) {
  return (uintmax_t)strtoull(nptr, endptr, base);
}

intmax_t wcstoimax(const wchar_t *nptr, wchar_t **endptr, int base) {
  size_t n = wstrlen_local(nptr);
  char *tmp = (char *)malloc(n + 1);
  char *nend = (char *)0;
  intmax_t out;

  if (!tmp)
    return 0;
  wtoa_local(nptr, tmp, n + 1);
  out = (intmax_t)strtoll(tmp, &nend, base);
  if (endptr) {
    size_t off = (size_t)(nend - tmp);
    *endptr = (wchar_t *)(nptr + off);
  }
  free(tmp);
  return out;
}

uintmax_t wcstoumax(const wchar_t *nptr, wchar_t **endptr, int base) {
  size_t n = wstrlen_local(nptr);
  char *tmp = (char *)malloc(n + 1);
  char *nend = (char *)0;
  uintmax_t out;

  if (!tmp)
    return 0;
  wtoa_local(nptr, tmp, n + 1);
  out = (uintmax_t)strtoull(tmp, &nend, base);
  if (endptr) {
    size_t off = (size_t)(nend - tmp);
    *endptr = (wchar_t *)(nptr + off);
  }
  free(tmp);
  return out;
}
