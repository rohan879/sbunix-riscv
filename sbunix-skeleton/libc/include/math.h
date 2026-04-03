#ifndef _MATH_H
#define _MATH_H

#include <stdint.h>

typedef double double_t;
typedef double float_t;

#define MATH_ERRNO 1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERRNO

#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4

#define FP_ILOGB0 (-2147483647 - 1)
#define FP_ILOGBNAN FP_ILOGB0

#define FP_FAST_FMA 0
#define FP_FAST_FMAF 0
#define FP_FAST_FMAL 0

static inline double __math_inf(void)
{
  union {
    double d;
    unsigned long long u;
  } u;
  u.u = 0x7ff0000000000000ULL;
  return u.d;
}

static inline double __math_nan(void)
{
  union {
    double d;
    unsigned long long u;
  } u;
  u.u = 0x7ff8000000000000ULL;
  return u.d;
}

#define INFINITY ((float)__math_inf())
#define NAN ((float)__math_nan())
#define HUGE_VAL (__math_inf())
#define HUGE_VALF ((float)__math_inf())
#define HUGE_VALL ((long double)__math_inf())

#define M_E 2.71828182845904523536
#define M_El 2.71828182845904523536L
#define M_LOG2E 1.44269504088896340736
#define M_LOG2El 1.44269504088896340736L
#define M_LOG10E 0.43429448190325182765
#define M_LOG10El 0.43429448190325182765L
#define M_LN2 0.69314718055994530942
#define M_LN2l 0.69314718055994530942L
#define M_LN10 2.30258509299404568402
#define M_LN10l 2.30258509299404568402L
#define M_PI 3.14159265358979323846
#define M_PIl 3.14159265358979323846L
#define M_PI_2 1.57079632679489661923
#define M_PI_2l 1.57079632679489661923L
#define M_PI_4 0.78539816339744830962
#define M_PI_4l 0.78539816339744830962L
#define M_1_PI 0.31830988618379067154
#define M_1_PIl 0.31830988618379067154L
#define M_2_PI 0.63661977236758134308
#define M_2_PIl 0.63661977236758134308L
#define M_2_SQRTPI 1.12837916709551257390
#define M_2_SQRTPIl 1.12837916709551257390L
#define M_1_SQRTPI 0.56418958354775628695
#define M_1_SQRTPIl 0.56418958354775628695L
#define M_SQRT2 1.41421356237309504880
#define M_SQRT2l 1.41421356237309504880L
#define M_SQRT1_2 0.70710678118654752440
#define M_SQRT1_2l 0.70710678118654752440L
#define M_SQRT3 1.73205080756887729353
#define M_SQRT3l 1.73205080756887729353L
#define M_SQRT1_3 0.57735026918962576451
#define M_SQRT1_3l 0.57735026918962576451L
#define M_PHI 1.61803398874989484820
#define M_PHIl 1.61803398874989484820L
#define M_EGAMMA 0.57721566490153286061
#define M_EGAMMAl 0.57721566490153286061L

extern int signgam;

int __math_isnan(double x);
int __math_isinf(double x);
int __math_isfinite(double x);

#define isnan(x) __math_isnan((double)(x))
#define isinf(x) __math_isinf((double)(x))
#define isfinite(x) __math_isfinite((double)(x))

static inline int __math_signbit(double x)
{
  union {
    double d;
    uint64_t u;
  } u;
  u.d = x;
  return (int)(u.u >> 63);
}

static inline int __math_fpclassify(double x)
{
  union {
    double d;
    uint64_t u;
  } u;
  u.d = x;
  uint64_t e = (u.u >> 52) & 0x7ffU;
  uint64_t f = u.u & 0x000fffffffffffffULL;
  if (e == 0x7ffU)
    return f ? FP_NAN : FP_INFINITE;
  if (e == 0)
    return f ? FP_SUBNORMAL : FP_ZERO;
  return FP_NORMAL;
}

#define signbit(x) __math_signbit((double)(x))
#define fpclassify(x) __math_fpclassify((double)(x))
#define isnormal(x) (fpclassify(x) == FP_NORMAL)

#define isgreater(x, y) ((x) > (y))
#define isgreaterequal(x, y) ((x) >= (y))
#define isless(x, y) ((x) < (y))
#define islessequal(x, y) ((x) <= (y))
#define islessgreater(x, y) ((x) < (y) || (x) > (y))
#define isunordered(x, y) (isnan(x) || isnan(y))

#ifdef __cplusplus
extern "C" {
#endif

double fabs(double x);
float fabsf(float x);
long double fabsl(long double x);

double fmod(double x, double y);
float fmodf(float x, float y);
long double fmodl(long double x, long double y);

double copysign(double x, double y);
float copysignf(float x, float y);
long double copysignl(long double x, long double y);

double fmax(double x, double y);
float fmaxf(float x, float y);
long double fmaxl(long double x, long double y);

double fmin(double x, double y);
float fminf(float x, float y);
long double fminl(long double x, long double y);

double ceil(double x);
float ceilf(float x);
long double ceill(long double x);

double floor(double x);
float floorf(float x);
long double floorl(long double x);

double round(double x);
float roundf(float x);
long double roundl(long double x);

double trunc(double x);
float truncf(float x);
long double truncl(long double x);

double frexp(double x, int *exp);
float frexpf(float x, int *exp);
long double frexpl(long double x, int *exp);

double ldexp(double x, int n);
float ldexpf(float x, int n);
long double ldexpl(long double x, int n);

double modf(double x, double *iptr);
float modff(float x, float *iptr);
long double modfl(long double x, long double *iptr);

double sqrt(double x);
float sqrtf(float x);
long double sqrtl(long double x);

double exp(double x);
float expf(float x);
long double expl(long double x);

double exp2(double x);
float exp2f(float x);
long double exp2l(long double x);

double expm1(double x);
float expm1f(float x);
long double expm1l(long double x);

double log(double x);
float logf(float x);
long double logl(long double x);

double log10(double x);
float log10f(float x);
long double log10l(long double x);

double log2(double x);
float log2f(float x);
long double log2l(long double x);

double log1p(double x);
float log1pf(float x);
long double log1pl(long double x);

double logb(double x);
float logbf(float x);
long double logbl(long double x);

double pow(double base, double exponent);
float powf(float base, float exponent);
long double powl(long double base, long double exponent);

double sin(double x);
float sinf(float x);
long double sinl(long double x);

double cos(double x);
float cosf(float x);
long double cosl(long double x);

double tan(double x);
float tanf(float x);
long double tanl(long double x);

double asin(double x);
float asinf(float x);
long double asinl(long double x);

double acos(double x);
float acosf(float x);
long double acosl(long double x);

double atan(double x);
float atanf(float x);
long double atanl(long double x);

double atan2(double y, double x);
float atan2f(float y, float x);
long double atan2l(long double y, long double x);

double sinh(double x);
float sinhf(float x);
long double sinhl(long double x);

double cosh(double x);
float coshf(float x);
long double coshl(long double x);

double tanh(double x);
float tanhf(float x);
long double tanhl(long double x);

double asinh(double x);
float asinhf(float x);
long double asinhl(long double x);

double acosh(double x);
float acoshf(float x);
long double acoshl(long double x);

double atanh(double x);
float atanhf(float x);
long double atanhl(long double x);

double remainder(double x, double y);
float remainderf(float x, float y);
long double remainderl(long double x, long double y);

double nearbyint(double x);
float nearbyintf(float x);
long double nearbyintl(long double x);

double rint(double x);
float rintf(float x);
long double rintl(long double x);

long lrint(double x);
long lrintf(float x);
long lrintl(long double x);

long long llrint(double x);
long long llrintf(float x);
long long llrintl(long double x);

long lround(double x);
long lroundf(float x);
long lroundl(long double x);

long long llround(double x);
long long llroundf(float x);
long long llroundl(long double x);

double scalbn(double x, int n);
float scalbnf(float x, int n);
long double scalbnl(long double x, int n);

double scalbln(double x, long n);
float scalblnf(float x, long n);
long double scalblnl(long double x, long n);

double cbrt(double x);
float cbrtf(float x);
long double cbrtl(long double x);

double hypot(double x, double y);
float hypotf(float x, float y);
long double hypotl(long double x, long double y);

double fdim(double x, double y);
float fdimf(float x, float y);
long double fdiml(long double x, long double y);

double fma(double x, double y, double z);
float fmaf(float x, float y, float z);
long double fmal(long double x, long double y, long double z);

double remquo(double x, double y, int *quo);
float remquof(float x, float y, int *quo);
long double remquol(long double x, long double y, int *quo);

double nextafter(double x, double y);
float nextafterf(float x, float y);
long double nextafterl(long double x, long double y);

double nexttoward(double x, long double y);
float nexttowardf(float x, long double y);
long double nexttowardl(long double x, long double y);

double nan(const char *tagp);
float nanf(const char *tagp);
long double nanl(const char *tagp);

double erf(double x);
float erff(float x);
long double erfl(long double x);

double erfc(double x);
float erfcf(float x);
long double erfcl(long double x);

double lgamma(double x);
float lgammaf(float x);
long double lgammal(long double x);

double tgamma(double x);
float tgammaf(float x);
long double tgammal(long double x);

int ilogb(double x);
int ilogbf(float x);
int ilogbl(long double x);

double j0(double x);
double j1(double x);
double jn(int n, double x);
double y0(double x);
double y1(double x);
double yn(int n, double x);

#ifdef __cplusplus
}
#endif

#endif
