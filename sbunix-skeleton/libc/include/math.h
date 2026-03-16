#ifndef _MATH_H
#define _MATH_H

/*
 * math.h — soft-float math library for SBUnix libc (rv64imac, lp64).
 *
 * All functions use double arithmetic.  On rv64imac the compiler
 * emits soft-float library calls which are resolved by libgcc.
 */

/* -------------------------------------------------------------------
 * Special-value macros (constructed via integer bit patterns)
 * ------------------------------------------------------------------- */

/* Positive infinity */
static inline double __math_inf(void)
{
    union { double d; unsigned long long u; } _u;
    _u.u = 0x7FF0000000000000ULL;
    return _u.d;
}

/* Quiet NaN */
static inline double __math_nan(void)
{
    union { double d; unsigned long long u; } _u;
    _u.u = 0x7FF8000000000000ULL;
    return _u.d;
}

/* Largest finite double */
static inline double __math_huge_val(void)
{
    union { double d; unsigned long long u; } _u;
    _u.u = 0x7FF0000000000000ULL;
    return _u.d;
}

#define INFINITY  __math_inf()
#define NAN       __math_nan()
#define HUGE_VAL  __math_huge_val()

#define M_PI      3.14159265358979323846
#define M_PI_2    1.57079632679489661923
#define M_PI_4    0.78539816339744830962
#define M_E       2.71828182845904523536
#define M_LN2     0.69314718055994530942
#define M_LN10    2.30258509299404568402
#define M_LOG2E   1.44269504088896340736
#define M_LOG10E  0.43429448190325182765
#define M_SQRT2   1.41421356237309504880

/* -------------------------------------------------------------------
 * Classification
 * ------------------------------------------------------------------- */

int    __math_isnan(double x);
int    __math_isinf(double x);
int    __math_isfinite(double x);

#define isnan(x)      __math_isnan(x)
#define isinf(x)      __math_isinf(x)
#define isfinite(x)   __math_isfinite(x)

/* -------------------------------------------------------------------
 * Basic operations
 * ------------------------------------------------------------------- */

double fabs(double x);
double fmod(double x, double y);
double copysign(double x, double y);
double fmax(double x, double y);
double fmin(double x, double y);

/* -------------------------------------------------------------------
 * Rounding
 * ------------------------------------------------------------------- */

double ceil(double x);
double floor(double x);
double round(double x);
double trunc(double x);

/* -------------------------------------------------------------------
 * Decomposition
 * ------------------------------------------------------------------- */

double frexp(double x, int *exp);
double ldexp(double x, int n);
double modf(double x, double *iptr);

/* -------------------------------------------------------------------
 * Power / root / exponential / logarithm
 * ------------------------------------------------------------------- */

double sqrt(double x);
double pow(double base, double exponent);
double exp(double x);
double log(double x);
double log10(double x);
double log2(double x);

/* -------------------------------------------------------------------
 * Trigonometric
 * ------------------------------------------------------------------- */

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

/* -------------------------------------------------------------------
 * Hyperbolic
 * ------------------------------------------------------------------- */

double sinh(double x);
double cosh(double x);
double tanh(double x);

/* -------------------------------------------------------------------
 * Additional POSIX / C99 math
 * ------------------------------------------------------------------- */

double remainder(double x, double y);
double nearbyint(double x);
double rint(double x);
long   lrint(double x);
long long llrint(double x);
double scalbn(double x, int n);
double cbrt(double x);
double hypot(double x, double y);
double expm1(double x);
double log1p(double x);

#endif /* _MATH_H */
