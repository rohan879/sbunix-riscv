#include <uchar.h>
#include <wchar.h>
#include <errno.h>
#include <string.h>

/* BF: Rewrote c16rtomb with UTF-16 surrogate pair handling.
 * Characters > U+FFFF require two char16_t values (high + low surrogate).
 * The pending high surrogate is stored in ps->pending_surrogate. */
size_t c16rtomb(char *restrict s, char16_t c16, mbstate_t *restrict ps)
{
  static mbstate_t internal = {0};
  if (!ps) ps = &internal;

  /* If we have a pending high surrogate, expect a low surrogate */
  if (ps->pending_surrogate) {
    uint32_t hi = ps->pending_surrogate;
    ps->pending_surrogate = 0;
    if (c16 >= 0xDC00 && c16 <= 0xDFFF) {
      /* Valid surrogate pair: decode to full codepoint */
      uint32_t cp = 0x10000 + ((hi - 0xD800) << 10) + (c16 - 0xDC00);
      return wcrtomb(s, (wchar_t)cp, ps);
    }
    /* Invalid: low surrogate expected but not received */
    errno = EILSEQ;
    return (size_t)-1;
  }

  /* High surrogate: stash and wait for low surrogate */
  if (c16 >= 0xD800 && c16 <= 0xDBFF) {
    ps->pending_surrogate = (uint32_t)c16;
    /* No output yet */
    if (!s) return 0;
    return 0;
  }

  /* Lone low surrogate is invalid */
  if (c16 >= 0xDC00 && c16 <= 0xDFFF) {
    errno = EILSEQ;
    return (size_t)-1;
  }

  return wcrtomb(s, (wchar_t)(uint32_t)c16, ps);
}

size_t c32rtomb(char *restrict s, char32_t c32, mbstate_t *restrict ps)
{
  return wcrtomb(s, (wchar_t)c32, ps);
}

/* BF: Rewrote mbrtoc16 with surrogate pair generation for codepoints
 * above U+FFFF.  The old version simply cast, which truncates BMP+
 * characters to 16-bit garbage. */
size_t mbrtoc16(char16_t *restrict pc16, const char *restrict s, size_t n,
                mbstate_t *restrict ps)
{
  static mbstate_t internal = {0};
  if (!ps) ps = &internal;

  /* If we have a pending low surrogate from a previous call, emit it */
  if (ps->pending_surrogate) {
    if (pc16) *pc16 = (char16_t)ps->pending_surrogate;
    ps->pending_surrogate = 0;
    return (size_t)-3; /* indicates second of surrogate pair */
  }

  wchar_t wc = 0;
  size_t r = mbrtowc(&wc, s, n, ps);
  if (r == (size_t)-1 || r == (size_t)-2)
    return r;

  uint32_t cp = (uint32_t)wc;
  if (cp > 0xFFFF) {
    /* Supplementary character: generate surrogate pair */
    uint32_t adjusted = cp - 0x10000;
    char16_t hi = (char16_t)(0xD800 + (adjusted >> 10));
    char16_t lo = (char16_t)(0xDC00 + (adjusted & 0x3FF));
    if (pc16) *pc16 = hi;
    ps->pending_surrogate = (uint32_t)lo;
    return r;
  }

  if (pc16) *pc16 = (char16_t)cp;
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
