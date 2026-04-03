#include <uchar.h>
#include <wchar.h>

size_t c16rtomb(char *restrict s, char16_t c16, mbstate_t *restrict ps)
{
  return wcrtomb(s, (wchar_t)c16, ps);
}

size_t c32rtomb(char *restrict s, char32_t c32, mbstate_t *restrict ps)
{
  return wcrtomb(s, (wchar_t)c32, ps);
}

size_t mbrtoc16(char16_t *restrict pc16, const char *restrict s, size_t n,
                mbstate_t *restrict ps)
{
  wchar_t wc = 0;
  size_t r = mbrtowc(&wc, s, n, ps);
  if (pc16)
    *pc16 = (char16_t)wc;
  return r;
}

size_t mbrtoc32(char32_t *restrict pc32, const char *restrict s, size_t n,
                mbstate_t *restrict ps)
{
  wchar_t wc = 0;
  size_t r = mbrtowc(&wc, s, n, ps);
  if (pc32)
    *pc32 = (char32_t)wc;
  return r;
}
