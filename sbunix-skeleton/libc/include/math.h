#ifndef _MATH_H
#define _MATH_H

/*
 * Minimal math library for SBUnix.
 *
 * On rv64imac (no F/D extensions), all floating-point operations use
 * GCC's soft-float routines from libgcc.  The functions below are
 * implemented in normal C with double/float types.
 */

/* Special value macros — use GCC builtins */
#define HUGE_VAL     __builtin_huge_val()
#define HUGE_VALF    __builtin_huge_valf()
#define INFINITY     __builtin_inf()
#define NAN          __builtin_nan("")

/* Classification macros */
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define signbit(x)    __builtin_signbit(x)

/* fpclassify support */
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, \
                                            FP_SUBNORMAL, FP_ZERO, (x))

/* Basic functions */
double fabs(double x);
double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double sqrt(double x);
double pow(double base, double exponent);
double log(double x);
double log10(double x);
double log2(double x);
double exp(double x);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double modf(double x, double *iptr);
double round(double x);
double trunc(double x);
double copysign(double x, double y);
double scalbn(double x, int n);

/* Float versions */
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float sqrtf(float x);
float fmodf(float x, float y);
float roundf(float x);
float truncf(float x);
float copysignf(float x, float y);

/* Trigonometric functions (basic approximations) */
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);

/* Hyperbolic (stubs for link compatibility) */
double sinh(double x);
double cosh(double x);
double tanh(double x);

#endif /* _MATH_H */
