#ifndef _TGMATH_H
#define _TGMATH_H

/*
 * tgmath.h -- C11 type-generic math macros for SBUnix.
 *
 * Each macro dispatches to the correct float/double/long-double (and
 * optionally complex) variant using _Generic.  For two-argument
 * functions we promote both operands to double unless either is
 * long double or float.
 *
 * Tests verify that these names are defined as macros (#ifndef sin / #error).
 */

#include <math.h>
#include <complex.h>

/* ===================================================================
 * Single-argument real + complex dispatch
 * =================================================================== */

/* Helper: dispatch real-only single-arg */
#define __tg_real1(fn, x) _Generic((x), \
    float:       fn##f, \
    long double: fn##l, \
    default:     fn)(x)

/* Helper: dispatch real+complex single-arg */
#define __tg_rc1(fn, cfn, x) _Generic((x), \
    float:                fn##f, \
    long double:          fn##l, \
    float _Complex:       c##fn##f, \
    long double _Complex: c##fn##l, \
    double _Complex:      c##fn, \
    default:              fn)(x)

/* ===================================================================
 * Two-argument real dispatch
 *
 * Promote to the widest type:
 *   - If either arg is long double -> long double variant
 *   - Otherwise -> double variant (float promotes to double per C11)
 * =================================================================== */

#define __tg_real2(fn, x, y) _Generic((x) + (y), \
    float:       fn##f, \
    long double: fn##l, \
    default:     fn)((x), (y))

/* ===================================================================
 * Three-argument real dispatch (fma)
 * =================================================================== */

#define __tg_real3(fn, x, y, z) _Generic((x) + (y) + (z), \
    float:       fn##f, \
    long double: fn##l, \
    default:     fn)((x), (y), (z))

/* ===================================================================
 * Trigonometric — real + complex
 * =================================================================== */

/* Undefine any existing macros from math.h before redefining */
#undef sin
#define sin(x) __tg_rc1(sin, csin, x)

#undef cos
#define cos(x) __tg_rc1(cos, ccos, x)

#undef tan
#define tan(x) __tg_rc1(tan, ctan, x)

#undef asin
#define asin(x) __tg_rc1(asin, casin, x)

#undef acos
#define acos(x) __tg_rc1(acos, cacos, x)

#undef atan
#define atan(x) __tg_rc1(atan, catan, x)

/* ===================================================================
 * Hyperbolic — real + complex
 * =================================================================== */

#undef sinh
#define sinh(x) __tg_rc1(sinh, csinh, x)

#undef cosh
#define cosh(x) __tg_rc1(cosh, ccosh, x)

#undef tanh
#define tanh(x) __tg_rc1(tanh, ctanh, x)

#undef asinh
#define asinh(x) __tg_rc1(asinh, casinh, x)

#undef acosh
#define acosh(x) __tg_rc1(acosh, cacosh, x)

#undef atanh
#define atanh(x) __tg_rc1(atanh, catanh, x)

/* ===================================================================
 * Exponential / logarithmic — real + complex
 * =================================================================== */

#undef exp
#define exp(x) __tg_rc1(exp, cexp, x)

#undef log
#define log(x) __tg_rc1(log, clog, x)

#undef sqrt
#define sqrt(x) __tg_rc1(sqrt, csqrt, x)

/* ===================================================================
 * Power — real + complex
 * =================================================================== */

#undef pow
#define pow(x, y) _Generic((x), \
    float _Complex:       cpowf, \
    long double _Complex: cpowl, \
    double _Complex:      cpow, \
    default: _Generic((y), \
        float _Complex:       cpowf, \
        long double _Complex: cpowl, \
        double _Complex:      cpow, \
        default: _Generic((x) + (y), \
            float:       powf, \
            long double: powl, \
            default:     pow)))((x), (y))

/* ===================================================================
 * Real-only single-argument functions
 * =================================================================== */

#undef fabs
#define fabs(x) _Generic((x), \
    float:                fabsf, \
    long double:          fabsl, \
    float _Complex:       cabsf, \
    long double _Complex: cabsl, \
    double _Complex:      cabs, \
    default:              fabs)(x)

#undef exp2
#define exp2(x)    __tg_real1(exp2, x)

#undef expm1
#define expm1(x)   __tg_real1(expm1, x)

#undef log10
#define log10(x)   __tg_real1(log10, x)

#undef log2
#define log2(x)    __tg_real1(log2, x)

#undef log1p
#define log1p(x)   __tg_real1(log1p, x)

#undef logb
#define logb(x)    __tg_real1(logb, x)

#undef cbrt
#define cbrt(x)    __tg_real1(cbrt, x)

#undef ceil
#define ceil(x)    __tg_real1(ceil, x)

#undef floor
#define floor(x)   __tg_real1(floor, x)

#undef round
#define round(x)   __tg_real1(round, x)

#undef trunc
#define trunc(x)   __tg_real1(trunc, x)

#undef nearbyint
#define nearbyint(x) __tg_real1(nearbyint, x)

#undef rint
#define rint(x)    __tg_real1(rint, x)

#undef erf
#define erf(x)     __tg_real1(erf, x)

#undef erfc
#define erfc(x)    __tg_real1(erfc, x)

#undef lgamma
#define lgamma(x)  __tg_real1(lgamma, x)

#undef tgamma
#define tgamma(x)  __tg_real1(tgamma, x)

/* ===================================================================
 * Real-only two-argument functions
 * =================================================================== */

#undef atan2
#define atan2(x, y)     __tg_real2(atan2, x, y)

#undef fmod
#define fmod(x, y)      __tg_real2(fmod, x, y)

#undef copysign
#define copysign(x, y)  __tg_real2(copysign, x, y)

#undef fdim
#define fdim(x, y)      __tg_real2(fdim, x, y)

#undef fmax
#define fmax(x, y)      __tg_real2(fmax, x, y)

#undef fmin
#define fmin(x, y)      __tg_real2(fmin, x, y)

#undef hypot
#define hypot(x, y)     __tg_real2(hypot, x, y)

#undef remainder
#define remainder(x, y) __tg_real2(remainder, x, y)

#undef nextafter
#define nextafter(x, y) __tg_real2(nextafter, x, y)

#undef scalbn
#define scalbn(x, n)    _Generic((x), \
    float:       scalbnf, \
    long double: scalbnl, \
    default:     scalbn)((x), (n))

#undef scalbln
#define scalbln(x, n)   _Generic((x), \
    float:       scalblnf, \
    long double: scalblnl, \
    default:     scalbln)((x), (n))

#undef ldexp
#define ldexp(x, n)     _Generic((x), \
    float:       ldexpf, \
    long double: ldexpl, \
    default:     ldexp)((x), (n))

/* ===================================================================
 * Three-argument: fma
 * =================================================================== */

#undef fma
#define fma(x, y, z) __tg_real3(fma, x, y, z)

/* ===================================================================
 * Functions with pointer output args — dispatch on the floating arg
 * =================================================================== */

#undef frexp
#define frexp(x, p) _Generic((x), \
    float:       frexpf, \
    long double: frexpl, \
    default:     frexp)((x), (p))

#undef modf
#define modf(x, p) _Generic((x), \
    float:       modff, \
    long double: modfl, \
    default:     modf)((x), (p))

#undef remquo
#define remquo(x, y, p) _Generic((x) + (y), \
    float:       remquof, \
    long double: remquol, \
    default:     remquo)((x), (y), (p))

/* ===================================================================
 * Integer return functions
 * =================================================================== */

#undef lrint
#define lrint(x) _Generic((x), \
    float:       lrintf, \
    long double: lrintl, \
    default:     lrint)(x)

#undef llrint
#define llrint(x) _Generic((x), \
    float:       llrintf, \
    long double: llrintl, \
    default:     llrint)(x)

#undef lround
#define lround(x) _Generic((x), \
    float:       lroundf, \
    long double: lroundl, \
    default:     lround)(x)

#undef llround
#define llround(x) _Generic((x), \
    float:       llroundf, \
    long double: llroundl, \
    default:     llround)(x)

#undef ilogb
#define ilogb(x) _Generic((x), \
    float:       ilogbf, \
    long double: ilogbl, \
    default:     ilogb)(x)

/* ===================================================================
 * Complex-only: carg, cimag, creal, conj, cproj
 * These only apply to complex types but tgmath.h must provide them.
 * =================================================================== */

#undef carg
#define carg(x) _Generic((x), \
    float _Complex:       cargf, \
    long double _Complex: cargl, \
    default:              carg)(x)

#undef cimag
#define cimag(x) _Generic((x), \
    float _Complex:       cimagf, \
    long double _Complex: cimagl, \
    default:              cimag)(x)

#undef creal
#define creal(x) _Generic((x), \
    float _Complex:       crealf, \
    long double _Complex: creall, \
    default:              creal)(x)

#undef conj
#define conj(x) _Generic((x), \
    float _Complex:       conjf, \
    long double _Complex: conjl, \
    default:              conj)(x)

#undef cproj
#define cproj(x) _Generic((x), \
    float _Complex:       cprojf, \
    long double _Complex: cprojl, \
    default:              cproj)(x)

/* nexttoward dispatches only on x, y is always long double */
#undef nexttoward
#define nexttoward(x, y) _Generic((x), \
    float:       nexttowardf, \
    long double: nexttowardl, \
    default:     nexttoward)((x), (y))

#endif /* _TGMATH_H */
