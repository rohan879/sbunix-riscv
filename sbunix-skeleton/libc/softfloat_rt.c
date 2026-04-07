#include <stdint.h>

typedef union {
  double d;
  uint64_t u;
} f64_u;

typedef union {
  float f;
  uint32_t u;
} f32_u;

static uint64_t shr_jam64(uint64_t v, unsigned dist)
{
  if (dist == 0) {
    return v;
  }
  if (dist >= 63) {
    return v ? 1 : 0;
  }
  return (v >> dist) | ((v << (64 - dist)) != 0);
}

static void mul_u64_u64(uint64_t a, uint64_t b, uint64_t* hi, uint64_t* lo)
{
  uint64_t a0 = (uint32_t)a;
  uint64_t a1 = a >> 32;
  uint64_t b0 = (uint32_t)b;
  uint64_t b1 = b >> 32;

  uint64_t p00 = a0 * b0;
  uint64_t p01 = a0 * b1;
  uint64_t p10 = a1 * b0;
  uint64_t p11 = a1 * b1;

  uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
  *lo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
  *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

static int bit_u128(uint64_t hi, uint64_t lo, unsigned bit)
{
  if (bit >= 64) {
    return (int)((hi >> (bit - 64)) & 1ULL);
  }
  return (int)((lo >> bit) & 1ULL);
}

static uint64_t shr_jam128_to64(uint64_t hi, uint64_t lo, unsigned dist)
{
  uint64_t out;
  int jam = 0;

  if (dist == 0) {
    return lo;
  }

  if (dist < 64) {
    out = (lo >> dist) | (hi << (64 - dist));
    jam = (lo << (64 - dist)) != 0;
    return out | (uint64_t)jam;
  }

  if (dist == 64) {
    return hi | (lo != 0);
  }

  if (dist < 128) {
    out = hi >> (dist - 64);
    jam = ((hi << (128 - dist)) != 0) || (lo != 0);
    return out | (uint64_t)jam;
  }

  return (hi || lo) ? 1 : 0;
}

static int clz64(uint64_t x)
{
  int n;

  if (x == 0) {
    return 64;
  }
  n = 0;
  if ((x & 0xFFFFFFFF00000000ULL) == 0) {
    n += 32;
    x <<= 32;
  }
  if ((x & 0xFFFF000000000000ULL) == 0) {
    n += 16;
    x <<= 16;
  }
  if ((x & 0xFF00000000000000ULL) == 0) {
    n += 8;
    x <<= 8;
  }
  if ((x & 0xF000000000000000ULL) == 0) {
    n += 4;
    x <<= 4;
  }
  if ((x & 0xC000000000000000ULL) == 0) {
    n += 2;
    x <<= 2;
  }
  if ((x & 0x8000000000000000ULL) == 0) {
    n += 1;
  }
  return n;
}

static uint64_t pack_df(int sign, int exp, uint64_t frac)
{
  return ((uint64_t)sign << 63) | ((uint64_t)exp << 52) | (frac & 0x000FFFFFFFFFFFFFULL);
}

static uint64_t qnan_df(void)
{
  return 0x7FF8000000000000ULL;
}

static int is_nan_df(uint64_t a)
{
  return ((a & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) &&
         (a & 0x000FFFFFFFFFFFFFULL);
}

static int is_inf_df(uint64_t a)
{
  return (a & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL;
}

static uint64_t round_pack_df(int sign, int exp, uint64_t sig)
{
  int round_bits;
  uint64_t frac;

  if (sig == 0) {
    return (uint64_t)sign << 63;
  }

  if (exp > 1023) {
    return pack_df(sign, 0x7FF, 0);
  }

  if (exp < -1022) {
    unsigned shift = (unsigned)(-1022 - exp);
    sig = shr_jam64(sig, shift);
    exp = -1022;
  }

  round_bits = (int)(sig & 0x7ULL);
  frac = sig >> 3;
  if (round_bits > 4 || (round_bits == 4 && (frac & 1ULL))) {
    frac++;
    if (frac & (1ULL << 53)) {
      frac >>= 1;
      exp++;
      if (exp > 1023) {
        return pack_df(sign, 0x7FF, 0);
      }
    }
  }

  if (exp == -1022 && frac < (1ULL << 52)) {
    return pack_df(sign, 0, frac);
  }

  return pack_df(sign, exp + 1023, frac & 0x000FFFFFFFFFFFFFULL);
}

double __adddf3(double a, double b)
{
  f64_u ua;
  f64_u ub;
  uint64_t aexpf;
  uint64_t bexpf;
  uint64_t afrac;
  uint64_t bfrac;
  int as;
  int bs;
  int ae;
  int be;
  uint64_t asig;
  uint64_t bsig;
  int exp;
  unsigned shift;
  int64_t vsum;
  uint64_t sig;
  f64_u out;

  ua.d = a;
  ub.d = b;

  if (is_nan_df(ua.u) || is_nan_df(ub.u)) {
    out.u = qnan_df();
    return out.d;
  }

  if (is_inf_df(ua.u) || is_inf_df(ub.u)) {
    if (is_inf_df(ua.u) && is_inf_df(ub.u) && ((ua.u ^ ub.u) >> 63)) {
      out.u = qnan_df();
      return out.d;
    }
    out.u = is_inf_df(ua.u) ? ua.u : ub.u;
    return out.d;
  }

  as = (int)(ua.u >> 63);
  bs = (int)(ub.u >> 63);
  aexpf = (ua.u >> 52) & 0x7FFULL;
  bexpf = (ub.u >> 52) & 0x7FFULL;
  afrac = ua.u & 0x000FFFFFFFFFFFFFULL;
  bfrac = ub.u & 0x000FFFFFFFFFFFFFULL;

  if (aexpf == 0 && afrac == 0) {
    return b;
  }
  if (bexpf == 0 && bfrac == 0) {
    return a;
  }

  if (aexpf == 0) {
    ae = -1022;
    asig = afrac;
  } else {
    ae = (int)aexpf - 1023;
    asig = (1ULL << 52) | afrac;
  }

  if (bexpf == 0) {
    be = -1022;
    bsig = bfrac;
  } else {
    be = (int)bexpf - 1023;
    bsig = (1ULL << 52) | bfrac;
  }

  asig <<= 3;
  bsig <<= 3;

  exp = ae;
  if (ae > be) {
    shift = (unsigned)(ae - be);
    bsig = shr_jam64(bsig, shift);
  } else if (be > ae) {
    shift = (unsigned)(be - ae);
    asig = shr_jam64(asig, shift);
    exp = be;
  }

  vsum = (as ? -(int64_t)asig : (int64_t)asig) + (bs ? -(int64_t)bsig : (int64_t)bsig);
  if (vsum == 0) {
    out.u = 0;
    return out.d;
  }

  if (vsum < 0) {
    as = 1;
    sig = (uint64_t)(-vsum);
  } else {
    as = 0;
    sig = (uint64_t)vsum;
  }

  if (sig & (1ULL << 56)) {
    sig = shr_jam64(sig, 1);
    exp++;
  } else {
    while (sig && sig < (1ULL << 55)) {
      sig <<= 1;
      exp--;
    }
  }

  out.u = round_pack_df(as, exp, sig);
  return out.d;
}

double __subdf3(double a, double b)
{
  f64_u ub;
  ub.d = b;
  ub.u ^= 1ULL << 63;
  return __adddf3(a, ub.d);
}

double __muldf3(double a, double b)
{
  f64_u ua;
  f64_u ub;
  uint64_t aexpf;
  uint64_t bexpf;
  uint64_t afrac;
  uint64_t bfrac;
  int as;
  int bs;
  int sign;
  int ae;
  int be;
  uint64_t asig;
  uint64_t bsig;
  uint64_t phi;
  uint64_t plo;
  unsigned top;
  unsigned shift;
  uint64_t sig;
  int exp;
  f64_u out;

  ua.d = a;
  ub.d = b;

  if (is_nan_df(ua.u) || is_nan_df(ub.u)) {
    out.u = qnan_df();
    return out.d;
  }

  if (is_inf_df(ua.u) || is_inf_df(ub.u)) {
    if (((ua.u & 0x7FFFFFFFFFFFFFFFULL) == 0) || ((ub.u & 0x7FFFFFFFFFFFFFFFULL) == 0)) {
      out.u = qnan_df();
      return out.d;
    }
    out.u = ((ua.u ^ ub.u) & (1ULL << 63)) | 0x7FF0000000000000ULL;
    return out.d;
  }

  if ((ua.u & 0x7FFFFFFFFFFFFFFFULL) == 0 || (ub.u & 0x7FFFFFFFFFFFFFFFULL) == 0) {
    out.u = (ua.u ^ ub.u) & (1ULL << 63);
    return out.d;
  }

  as = (int)(ua.u >> 63);
  bs = (int)(ub.u >> 63);
  sign = as ^ bs;

  aexpf = (ua.u >> 52) & 0x7FFULL;
  bexpf = (ub.u >> 52) & 0x7FFULL;
  afrac = ua.u & 0x000FFFFFFFFFFFFFULL;
  bfrac = ub.u & 0x000FFFFFFFFFFFFFULL;

  if (aexpf == 0) {
    ae = -1022;
    asig = afrac;
    while ((asig & (1ULL << 52)) == 0) {
      asig <<= 1;
      ae--;
    }
  } else {
    ae = (int)aexpf - 1023;
    asig = (1ULL << 52) | afrac;
  }

  if (bexpf == 0) {
    be = -1022;
    bsig = bfrac;
    while ((bsig & (1ULL << 52)) == 0) {
      bsig <<= 1;
      be--;
    }
  } else {
    be = (int)bexpf - 1023;
    bsig = (1ULL << 52) | bfrac;
  }

  mul_u64_u64(asig, bsig, &phi, &plo);
  top = bit_u128(phi, plo, 105) ? 105 : 104;
  shift = top - 55;
  sig = shr_jam128_to64(phi, plo, shift);
  exp = ae + be + (int)top - 104;

  out.u = round_pack_df(sign, exp, sig);
  return out.d;
}

double __divdf3(double a, double b)
{
  f64_u ua;
  f64_u ub;
  int sign;
  int ae;
  int be;
  uint64_t aexpf;
  uint64_t bexpf;
  uint64_t afrac;
  uint64_t bfrac;
  uint64_t asig;
  uint64_t bsig;
  int exp;
  uint64_t q;
  uint64_t rem;
  int i;
  f64_u out;

  ua.d = a;
  ub.d = b;

  if (is_nan_df(ua.u) || is_nan_df(ub.u)) {
    out.u = qnan_df();
    return out.d;
  }

  if ((ub.u & 0x7FFFFFFFFFFFFFFFULL) == 0) {
    if ((ua.u & 0x7FFFFFFFFFFFFFFFULL) == 0) {
      out.u = qnan_df();
      return out.d;
    }
    out.u = ((ua.u ^ ub.u) & (1ULL << 63)) | 0x7FF0000000000000ULL;
    return out.d;
  }

  if ((ua.u & 0x7FFFFFFFFFFFFFFFULL) == 0) {
    out.u = (ua.u ^ ub.u) & (1ULL << 63);
    return out.d;
  }

  if (is_inf_df(ua.u)) {
    if (is_inf_df(ub.u)) {
      out.u = qnan_df();
      return out.d;
    }
    out.u = ((ua.u ^ ub.u) & (1ULL << 63)) | 0x7FF0000000000000ULL;
    return out.d;
  }

  if (is_inf_df(ub.u)) {
    out.u = (ua.u ^ ub.u) & (1ULL << 63);
    return out.d;
  }

  sign = (int)((ua.u ^ ub.u) >> 63);
  aexpf = (ua.u >> 52) & 0x7FFULL;
  bexpf = (ub.u >> 52) & 0x7FFULL;
  afrac = ua.u & 0x000FFFFFFFFFFFFFULL;
  bfrac = ub.u & 0x000FFFFFFFFFFFFFULL;

  if (aexpf == 0) {
    ae = -1022;
    asig = afrac;
    while ((asig & (1ULL << 52)) == 0) {
      asig <<= 1;
      ae--;
    }
  } else {
    ae = (int)aexpf - 1023;
    asig = (1ULL << 52) | afrac;
  }

  if (bexpf == 0) {
    be = -1022;
    bsig = bfrac;
    while ((bsig & (1ULL << 52)) == 0) {
      bsig <<= 1;
      be--;
    }
  } else {
    be = (int)bexpf - 1023;
    bsig = (1ULL << 52) | bfrac;
  }

  exp = ae - be;

  /*
   * Binary long division producing 56-bit quotient.
   *
   * FIX: The original code started with rem = asig (~2^53) and
   * shifted left 56 times, causing 64-bit overflow after ~10 shifts.
   * Fixed by normalizing both significands to the top of the word
   * first, then extracting quotient bits from high to low.
   *
   * Normalize so bsig has its MSB at bit 62 (leaving bit 63 for
   * the doubled remainder). This keeps rem < 2*bsig < 2^63
   * throughout the loop, preventing overflow.
   */
  /* Count leading zeros to normalize bsig to bit 62 */
  int nlz = 0;
  {
    uint64_t tmp = bsig;
    while (!(tmp & (1ULL << 62))) {
      tmp <<= 1;
      nlz++;
    }
  }
  bsig <<= nlz;
  asig <<= nlz;

  /* Adjust if asig < bsig (after normalization) */
  if (asig < bsig) {
    asig <<= 1;
    exp--;
  }

  q = 0;
  rem = asig;
  for (i = 0; i < 56; i++) {
    q <<= 1;
    if (rem >= bsig) {
      rem -= bsig;
      q |= 1ULL;
    }
    rem <<= 1;
  }
  /* Final comparison for sticky bit (rounding) */
  if (rem >= bsig) {
    q |= 1ULL;
  } else if (rem != 0) {
    q |= 1ULL;
  }

  out.u = round_pack_df(sign, exp, q);
  return out.d;
}

static int dcmp(double a, double b)
{
  f64_u ua;
  f64_u ub;
  int sa;
  int sb;

  ua.d = a;
  ub.d = b;

  if (is_nan_df(ua.u) || is_nan_df(ub.u)) {
    return 1;
  }

  if ((ua.u << 1) == 0 && (ub.u << 1) == 0) {
    return 0;
  }
  if (ua.u == ub.u) {
    return 0;
  }

  sa = (int)(ua.u >> 63);
  sb = (int)(ub.u >> 63);
  if (sa != sb) {
    return sa ? -1 : 1;
  }

  if (!sa) {
    return ua.u < ub.u ? -1 : 1;
  }
  return ua.u > ub.u ? -1 : 1;
}

int __eqdf2(double a, double b)
{
  return dcmp(a, b) == 0 ? 0 : 1;
}

int __nedf2(double a, double b)
{
  return dcmp(a, b) == 0 ? 0 : 1;
}

/*
 * GCC soft-float comparison return conventions:
 *   __ltdf2: returns < 0 if a < b, else >= 0. NaN -> >= 0 (not less).
 *   __gtdf2: returns > 0 if a > b, else <= 0. NaN -> <= 0 (not greater).
 *   __ledf2: returns <= 0 if a <= b, else > 0. NaN -> > 0 (not le).
 *   __gedf2: returns >= 0 if a >= b, else < 0. NaN -> < 0 (not ge).
 *
 * FIX: The old dcmp returned 1 for NaN which made __gtdf2 return 1
 * (meaning "greater"), violating IEEE 754 NaN ordering rules.
 * Now each function checks for NaN explicitly and returns the correct
 * "unordered" value per GCC conventions.
 */
int __ltdf2(double a, double b)
{
  f64_u ua, ub;
  ua.d = a; ub.d = b;
  /* NaN: return >= 0 so (a < b) is false */
  if (is_nan_df(ua.u) || is_nan_df(ub.u))
    return 1;
  return dcmp(a, b) < 0 ? -1 : 0;
}

int __gtdf2(double a, double b)
{
  f64_u ua, ub;
  ua.d = a; ub.d = b;
  /* NaN: return <= 0 so (a > b) is false */
  if (is_nan_df(ua.u) || is_nan_df(ub.u))
    return -1;
  return dcmp(a, b) > 0 ? 1 : 0;
}

int __ledf2(double a, double b)
{
  f64_u ua, ub;
  ua.d = a; ub.d = b;
  /* NaN: return > 0 so (a <= b) is false */
  if (is_nan_df(ua.u) || is_nan_df(ub.u))
    return 1;
  return dcmp(a, b);
}

int __gedf2(double a, double b)
{
  f64_u ua, ub;
  ua.d = a; ub.d = b;
  /* NaN: return < 0 so (a >= b) is false */
  if (is_nan_df(ua.u) || is_nan_df(ub.u))
    return -1;
  return dcmp(a, b);
}

double __extendsfdf2(float a)
{
  f32_u uf;
  f64_u ud;
  uint32_t sign;
  uint32_t exp;
  uint32_t frac;

  uf.f = a;
  sign = uf.u >> 31;
  exp = (uf.u >> 23) & 0xFFU;
  frac = uf.u & 0x7FFFFFU;

  if (exp == 0xFFU) {
    ud.u = ((uint64_t)sign << 63) | 0x7FF0000000000000ULL | ((uint64_t)frac << 29);
    return ud.d;
  }

  if (exp == 0) {
    if (frac == 0) {
      ud.u = (uint64_t)sign << 63;
      return ud.d;
    }
    {
      int e = -126;
      uint32_t m = frac;
      while ((m & 0x800000U) == 0) {
        m <<= 1;
        e--;
      }
      m &= 0x7FFFFFU;
      ud.u = ((uint64_t)sign << 63) | ((uint64_t)(e + 1023) << 52) | ((uint64_t)m << 29);
      return ud.d;
    }
  }

  ud.u = ((uint64_t)sign << 63) | ((uint64_t)(exp - 127 + 1023) << 52) | ((uint64_t)frac << 29);
  return ud.d;
}

float __truncdfsf2(double a)
{
  f64_u ud;
  f32_u uf;
  uint64_t sign;
  int exp;
  uint64_t frac;
  uint64_t sig;
  int e32;
  uint32_t frac32;
  uint64_t round_bits;

  ud.d = a;
  sign = ud.u >> 63;
  exp = (int)((ud.u >> 52) & 0x7FFULL);
  frac = ud.u & 0x000FFFFFFFFFFFFFULL;

  if (exp == 0x7FF) {
    uf.u = ((uint32_t)sign << 31) | 0x7F800000U | (frac ? 0x00400000U : 0);
    return uf.f;
  }

  if (exp == 0 && frac == 0) {
    uf.u = (uint32_t)sign << 31;
    return uf.f;
  }

  if (exp == 0) {
    int e = -1022;
    sig = frac;
    while ((sig & (1ULL << 52)) == 0) {
      sig <<= 1;
      e--;
    }
    frac = sig & 0x000FFFFFFFFFFFFFULL;
    exp = e + 1023;
  }

  e32 = (exp - 1023) + 127;
  sig = (1ULL << 52) | frac;

  if (e32 >= 0xFF) {
    uf.u = ((uint32_t)sign << 31) | 0x7F800000U;
    return uf.f;
  }

  if (e32 <= 0) {
    unsigned shift = (unsigned)(1 - e32 + 29);
    uint64_t rs = shr_jam64(sig, shift);
    round_bits = rs & 0x7ULL;
    frac32 = (uint32_t)(rs >> 3);
    if (round_bits > 4 || (round_bits == 4 && (frac32 & 1U))) {
      frac32++;
    }
    uf.u = ((uint32_t)sign << 31) | frac32;
    return uf.f;
  }

  {
    uint64_t rs = shr_jam64(sig, 52 - 23 - 3);
    round_bits = rs & 0x7ULL;
    frac32 = (uint32_t)(rs >> 3);
    if (round_bits > 4 || (round_bits == 4 && (frac32 & 1U))) {
      frac32++;
      if (frac32 & (1U << 24)) {
        frac32 >>= 1;
        e32++;
        if (e32 >= 0xFF) {
          uf.u = ((uint32_t)sign << 31) | 0x7F800000U;
          return uf.f;
        }
      }
    }
  }

  uf.u = ((uint32_t)sign << 31) | ((uint32_t)e32 << 23) | (frac32 & 0x7FFFFFU);
  return uf.f;
}

double __floatdidf(long long i);

double __floatsidf(int i)
{
  return __floatdidf((long long)i);
}

double __floatdidf(long long i)
{
  f64_u out;
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
  if (i < 0) {
    u = (uint64_t)(-(i + 1)) + 1ULL;
  } else {
    u = (uint64_t)i;
  }

  msb = 63 - clz64(u);

  if (msb <= 52) {
    frac = (u << (52 - msb)) & 0x000FFFFFFFFFFFFFULL;
  } else {
    uint64_t r = shr_jam64(u, (unsigned)(msb - 52 - 3));
    uint64_t rb = r & 0x7ULL;
    uint64_t m = r >> 3;
    if (rb > 4 || (rb == 4 && (m & 1ULL))) {
      m++;
      if (m & (1ULL << 53)) {
        m >>= 1;
        msb++;
      }
    }
    frac = m & 0x000FFFFFFFFFFFFFULL;
  }

  bits = (sign << 63) | ((uint64_t)(msb + 1023) << 52) | frac;
  out.u = bits;
  return out.d;
}

/* BF: Added __floatundidf (unsigned long long -> double) for printf
 * float formatting.  Same algorithm as __floatdidf but without sign
 * handling since the input is unsigned. */
double __floatundidf(unsigned long long u)
{
  f64_u out;
  uint64_t frac;
  uint64_t bits;
  int msb;

  if (u == 0) {
    out.u = 0;
    return out.d;
  }

  msb = 63 - clz64(u);

  if (msb <= 52) {
    frac = (u << (52 - msb)) & 0x000FFFFFFFFFFFFFULL;
  } else {
    uint64_t r = shr_jam64(u, (unsigned)(msb - 52 - 3));
    uint64_t rb = r & 0x7ULL;
    uint64_t m = r >> 3;
    if (rb > 4 || (rb == 4 && (m & 1ULL))) {
      m++;
      if (m & (1ULL << 53)) {
        msb++;
        m >>= 1;
      }
    }
    frac = m & 0x000FFFFFFFFFFFFFULL;
  }

  bits = ((uint64_t)(msb + 1023) << 52) | frac;
  out.u = bits;
  return out.d;
}

long long __fixdfdi(double a)
{
  f64_u ua;
  int sign;
  int exp;
  uint64_t frac;
  uint64_t sig;
  int shift;
  uint64_t mag;

  ua.d = a;
  sign = (int)(ua.u >> 63);
  exp = (int)((ua.u >> 52) & 0x7FFULL);
  frac = ua.u & 0x000FFFFFFFFFFFFFULL;

  if (exp == 0x7FF) {
    return sign ? (long long)0x8000000000000000LL : (long long)0x7FFFFFFFFFFFFFFFLL;
  }

  if (exp == 0) {
    return 0;
  }

  exp = exp - 1023;
  if (exp < 0) {
    return 0;
  }

  sig = (1ULL << 52) | frac;
  shift = exp - 52;
  if (shift >= 0) {
    if (shift >= 11) {
      return sign ? (long long)0x8000000000000000LL : (long long)0x7FFFFFFFFFFFFFFFLL;
    }
    mag = sig << shift;
  } else {
    mag = sig >> (-shift);
  }

  if (sign) {
    if (mag > 0x8000000000000000ULL) {
      return (long long)0x8000000000000000LL;
    }
    if (mag == 0x8000000000000000ULL) {
      return (long long)0x8000000000000000LL;
    }
    return -(long long)mag;
  }

  if (mag > 0x7FFFFFFFFFFFFFFFULL) {
    return (long long)0x7FFFFFFFFFFFFFFFLL;
  }
  return (long long)mag;
}

unsigned long long __fixunsdfdi(double a)
{
  f64_u ua;
  int sign;
  int exp;
  uint64_t frac;
  uint64_t sig;
  int shift;
  uint64_t mag;

  ua.d = a;
  sign = (int)(ua.u >> 63);
  exp = (int)((ua.u >> 52) & 0x7FFULL);
  frac = ua.u & 0x000FFFFFFFFFFFFFULL;

  if (exp == 0x7FF) {
    if (frac != 0) {
      return 0;
    }
    return sign ? 0 : 0xFFFFFFFFFFFFFFFFULL;
  }

  if (sign || exp == 0) {
    return 0;
  }

  exp = exp - 1023;
  if (exp < 0) {
    return 0;
  }

  sig = (1ULL << 52) | frac;
  shift = exp - 52;
  if (shift >= 0) {
    if (shift > 11) {
      return 0xFFFFFFFFFFFFFFFFULL;
    }
    mag = sig << shift;
  } else {
    mag = sig >> (-shift);
  }

  return mag;
}

long long __fixsfdi(float a)
{
  return __fixdfdi(__extendsfdf2(a));
}

unsigned long long __fixunssfdi(float a)
{
  return __fixunsdfdi(__extendsfdf2(a));
}

/* BF: Added __fixdfsi (double -> signed int) for printf float
 * formatting.  Delegates to __fixdfdi then narrows to int. */
int __fixdfsi(double a)
{
  long long v = __fixdfdi(a);
  if (v > 2147483647LL) return 2147483647;
  if (v < -2147483648LL) return -2147483647 - 1;
  return (int)v;
}

unsigned int __fixunsdfsi(double a)
{
  unsigned long long v = __fixunsdfdi(a);
  if (v > 0xFFFFFFFFULL) {
    return 0xFFFFFFFFU;
  }
  return (unsigned int)v;
}

unsigned int __fixunssfsi(float a)
{
  return __fixunsdfsi(__extendsfdf2(a));
}

float __addsf3(float a, float b)
{
  double da = __extendsfdf2(a);
  double db = __extendsfdf2(b);
  return __truncdfsf2(__adddf3(da, db));
}

float __subsf3(float a, float b)
{
  double da = __extendsfdf2(a);
  double db = __extendsfdf2(b);
  return __truncdfsf2(__subdf3(da, db));
}

float __mulsf3(float a, float b)
{
  double da = __extendsfdf2(a);
  double db = __extendsfdf2(b);
  return __truncdfsf2(__muldf3(da, db));
}

float __divsf3(float a, float b)
{
  double da = __extendsfdf2(a);
  double db = __extendsfdf2(b);
  return __truncdfsf2(__divdf3(da, db));
}

int __ltsf2(float a, float b)
{
  return __ltdf2(__extendsfdf2(a), __extendsfdf2(b));
}

int __gtsf2(float a, float b)
{
  return __gtdf2(__extendsfdf2(a), __extendsfdf2(b));
}

int __nesf2(float a, float b)
{
  return __nedf2(__extendsfdf2(a), __extendsfdf2(b));
}

typedef union {
  long double ld;
  struct {
    uint64_t lo;
    uint64_t hi;
  } w;
} tf_u;

static int tf_is_nan(tf_u x)
{
  uint16_t e = (uint16_t)((x.w.hi >> 48) & 0x7FFFU);
  uint64_t fh = x.w.hi & 0x0000FFFFFFFFFFFFULL;
  return e == 0x7FFFU && (fh || x.w.lo);
}

static double tf_to_df(long double a)
{
  tf_u t;
  f64_u d;
  int sign;
  int e;
  uint64_t fh;
  uint64_t fl;
  uint64_t m53;
  uint64_t rem;
  int de;

  t.ld = a;
  sign = (int)(t.w.hi >> 63);
  e = (int)((t.w.hi >> 48) & 0x7FFFU);
  fh = t.w.hi & 0x0000FFFFFFFFFFFFULL;
  fl = t.w.lo;

  if (e == 0x7FFF) {
    if (fh || fl) {
      d.u = qnan_df();
    } else {
      d.u = ((uint64_t)sign << 63) | 0x7FF0000000000000ULL;
    }
    return d.d;
  }

  if (e == 0 && fh == 0 && fl == 0) {
    d.u = (uint64_t)sign << 63;
    return d.d;
  }

  if (e == 0) {
    int shift = 0;
    while ((fh & (1ULL << 47)) == 0) {
      fh = (fh << 1) | (fl >> 63);
      fl <<= 1;
      shift++;
      if (shift > 120) {
        d.u = (uint64_t)sign << 63;
        return d.d;
      }
    }
    e = 1 - shift;
  }

  m53 = (((1ULL << 48) | fh) << 4) | (fl >> 60);
  rem = fl & 0x0FFFFFFFFFFFFFFFULL;
  if (rem > (1ULL << 59) || (rem == (1ULL << 59) && (m53 & 1ULL))) {
    m53++;
    if (m53 == (1ULL << 53)) {
      m53 >>= 1;
      e++;
    }
  }

  de = (e - 16383) + 1023;
  if (de >= 0x7FF) {
    d.u = ((uint64_t)sign << 63) | 0x7FF0000000000000ULL;
    return d.d;
  }
  if (de <= 0) {
    d.u = (uint64_t)sign << 63;
    return d.d;
  }

  d.u = ((uint64_t)sign << 63) | ((uint64_t)de << 52) | (m53 & 0x000FFFFFFFFFFFFFULL);
  return d.d;
}

static long double df_to_tf(double a)
{
  f64_u d;
  tf_u t;
  int sign;
  int e;
  uint64_t frac;
  int te;

  d.d = a;
  sign = (int)(d.u >> 63);
  e = (int)((d.u >> 52) & 0x7FFULL);
  frac = d.u & 0x000FFFFFFFFFFFFFULL;

  if (e == 0x7FF) {
    t.w.hi = ((uint64_t)sign << 63) | ((uint64_t)0x7FFF << 48);
    t.w.lo = 0;
    if (frac) {
      t.w.hi |= 1ULL;
    }
    return t.ld;
  }

  if (e == 0 && frac == 0) {
    t.w.hi = (uint64_t)sign << 63;
    t.w.lo = 0;
    return t.ld;
  }

  if (e == 0) {
    while ((frac & (1ULL << 52)) == 0) {
      frac <<= 1;
      e--;
    }
    e++;
    frac &= 0x000FFFFFFFFFFFFFULL;
  }

  te = (e - 1023) + 16383;
  t.w.hi = ((uint64_t)sign << 63) | ((uint64_t)te << 48) | (frac >> 4);
  t.w.lo = (frac & 0xFULL) << 60;
  return t.ld;
}

int __netf2(long double a, long double b)
{
  tf_u ta;
  tf_u tb;

  ta.ld = a;
  tb.ld = b;
  if (tf_is_nan(ta) || tf_is_nan(tb)) {
    return 1;
  }
  if (((ta.w.hi << 1) == 0 && ta.w.lo == 0) && ((tb.w.hi << 1) == 0 && tb.w.lo == 0)) {
    return 0;
  }
  return (ta.w.hi != tb.w.hi) || (ta.w.lo != tb.w.lo);
}

int __lttf2(long double a, long double b)
{
  double da = tf_to_df(a);
  double db = tf_to_df(b);
  return __ltdf2(da, db);
}

int __gttf2(long double a, long double b)
{
  double da = tf_to_df(a);
  double db = tf_to_df(b);
  return __gtdf2(da, db);
}

long double __extenddftf2(double a)
{
  return df_to_tf(a);
}

double __trunctfdf2(long double a)
{
  return tf_to_df(a);
}

long double __addtf3(long double a, long double b)
{
  return df_to_tf(__adddf3(tf_to_df(a), tf_to_df(b)));
}

long double __subtf3(long double a, long double b)
{
  return df_to_tf(__subdf3(tf_to_df(a), tf_to_df(b)));
}

long double __multf3(long double a, long double b)
{
  return df_to_tf(__muldf3(tf_to_df(a), tf_to_df(b)));
}

long double __divtf3(long double a, long double b)
{
  return df_to_tf(__divdf3(tf_to_df(a), tf_to_df(b)));
}

double _Complex __muldc3(double a, double b, double c, double d)
{
  double _Complex z;
  double re = __subdf3(__muldf3(a, c), __muldf3(b, d));
  double im = __adddf3(__muldf3(a, d), __muldf3(b, c));
  __real__ z = re;
  __imag__ z = im;
  return z;
}

double _Complex __divdc3(double a, double b, double c, double d)
{
  double _Complex z;
  double den = __adddf3(__muldf3(c, c), __muldf3(d, d));
  double re = __divdf3(__adddf3(__muldf3(a, c), __muldf3(b, d)), den);
  double im = __divdf3(__subdf3(__muldf3(b, c), __muldf3(a, d)), den);
  __real__ z = re;
  __imag__ z = im;
  return z;
}
