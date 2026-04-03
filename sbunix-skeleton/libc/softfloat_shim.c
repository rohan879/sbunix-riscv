#include <stdint.h>
#include <string.h>

typedef union {
  double d;
  uint64_t u;
} u64d_t;

typedef union {
  float f;
  uint32_t u;
} u32f_t;

static int is_nan_u64(uint64_t u) {
  return ((u & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL) &&
         (u & 0x000fffffffffffffULL);
}

static int dcmp(double a, double b) {
  u64d_t ua;
  u64d_t ub;
  int sa;
  int sb;

  ua.d = a;
  ub.d = b;

  if (is_nan_u64(ua.u) || is_nan_u64(ub.u))
    return 1;

  if ((ua.u << 1) == 0 && (ub.u << 1) == 0)
    return 0;

  if (ua.u == ub.u)
    return 0;

  sa = (int)(ua.u >> 63);
  sb = (int)(ub.u >> 63);
  if (sa != sb)
    return sa ? -1 : 1;

  if (!sa)
    return ua.u < ub.u ? -1 : 1;

  return ua.u > ub.u ? -1 : 1;
}

int __eqdf2(double a, double b) {
  return dcmp(a, b) == 0 ? 0 : 1;
}

int __nedf2(double a, double b) {
  return dcmp(a, b) == 0 ? 0 : 1;
}

int __ltdf2(double a, double b) {
  return dcmp(a, b) < 0 ? -1 : 1;
}

int __gtdf2(double a, double b) {
  return dcmp(a, b) > 0 ? 1 : -1;
}

int __ledf2(double a, double b) {
  return dcmp(a, b);
}

int __gedf2(double a, double b) {
  return dcmp(a, b);
}

double __extendsfdf2(float a) {
  u32f_t uf;
  u64d_t ud;
  uint32_t sign;
  uint32_t exp;
  uint32_t frac;

  uf.f = a;
  sign = uf.u >> 31;
  exp = (uf.u >> 23) & 0xffU;
  frac = uf.u & 0x7fffffU;

  if (exp == 0xffU) {
    ud.u = ((uint64_t)sign << 63) | 0x7ff0000000000000ULL |
           ((uint64_t)frac << 29);
    return ud.d;
  }

  if (exp == 0) {
    if (frac == 0) {
      ud.u = (uint64_t)sign << 63;
      return ud.d;
    }

    int e = -126;
    uint32_t m = frac;
    while ((m & 0x800000U) == 0) {
      m <<= 1;
      e--;
    }
    m &= 0x7fffffU;
    ud.u = ((uint64_t)sign << 63) |
           ((uint64_t)(e + 1023) << 52) |
           ((uint64_t)m << 29);
    return ud.d;
  }

  ud.u = ((uint64_t)sign << 63) |
         ((uint64_t)(exp - 127 + 1023) << 52) |
         ((uint64_t)frac << 29);
  return ud.d;
}

double __floatdidf(long long i) {
  u64d_t out;
  uint64_t u;
  uint64_t frac;
  uint64_t bits;
  uint64_t sign;
  int msb;

  if (i == 0) {
    out.u = 0;
    return out.d;
  }

  sign = i < 0 ? 1ULL : 0ULL;
  if (i < 0)
    u = (uint64_t)(-(i + 1)) + 1ULL;
  else
    u = (uint64_t)i;

  msb = 63;
  while (msb > 0 && ((u >> msb) & 1ULL) == 0)
    msb--;

  if (msb <= 52)
    frac = (u << (52 - msb)) & 0x000fffffffffffffULL;
  else
    frac = (u >> (msb - 52)) & 0x000fffffffffffffULL;

  bits = (sign << 63) | ((uint64_t)(msb + 1023) << 52) | frac;
  out.u = bits;
  return out.d;
}

double __subdf3(double a, double b) {
  u64d_t ua;
  u64d_t ub;
  ua.d = a;
  ub.d = b;

  if ((ub.u << 1) == 0)
    return a;
  if ((ua.u << 1) == 0) {
    ub.u ^= (1ULL << 63);
    return ub.d;
  }
  if (__eqdf2(a, b) == 0)
    return 0.0;
  return 0.0;
}

double __muldf3(double a, double b) {
  u64d_t ua;
  u64d_t ub;
  ua.d = a;
  ub.d = b;

  if ((ua.u << 1) == 0 || (ub.u << 1) == 0)
    return 0.0;
  if (ub.u == 0x3ff0000000000000ULL)
    return a;
  if (ua.u == 0x3ff0000000000000ULL)
    return b;
  return 0.0;
}

int __netf2(long double a, long double b) {
  return memcmp(&a, &b, sizeof(long double)) != 0;
}
