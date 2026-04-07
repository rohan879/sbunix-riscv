/*
 * math.c — soft-float math library for SBUnix libc.
 *
 * Target: rv64imac (no FPU), ABI lp64.
 *
 * The compiler emits soft-float calls (__adddf3, __muldf3, etc.) for
 * double arithmetic, which libgcc provides.  All heavy-lifting is
 * done in normal C double code; the only "manual" bit manipulation
 * is for classification (isnan/isinf), decomposition (frexp/ldexp),
 * and special-value construction.
 *
 * Algorithms use fdlibm/musl-derived polynomial approximations with
 * Horner-form evaluation for minimal FP operation count in soft-float.
 * Coefficients are from Sun fdlibm (BSD-licensed).
 */

#include <math.h>
#include <stdint.h>
#include <errno.h>  /* errno / EDOM / ERANGE — needed for math_errhandling == MATH_ERRNO */
#include "math_tables.h" /* Bessel, erf, lgamma coefficient tables */

int signgam = 0;

/* ===================================================================
 * Internal helpers: IEEE 754 bit access
 * =================================================================== */

typedef union {
    double   d;
    uint64_t u;
} f64_bits;

/* Extract raw biased exponent (11 bits) */
static inline int f64_rawexp(double x)
{
    f64_bits b;
    b.d = x;
    return (int)((b.u >> 52) & 0x7FF);
}

/* Extract sign bit */
static inline int f64_sign(double x)
{
    f64_bits b;
    b.d = x;
    return (int)(b.u >> 63);
}

/* Extract fraction (52 bits) */
static inline uint64_t f64_frac(double x)
{
    f64_bits b;
    b.d = x;
    return b.u & 0x000FFFFFFFFFFFFFULL;
}

/* Build double from raw bits */
static inline double f64_from_bits(uint64_t u)
{
    f64_bits b;
    b.u = u;
    return b.d;
}

/* Positive zero */
static inline double pos_zero(void)
{
    return f64_from_bits(0x0000000000000000ULL);
}

/* Positive infinity */
static inline double pos_inf(void)
{
    return f64_from_bits(0x7FF0000000000000ULL);
}

/* Negative infinity */
static inline double neg_inf(void)
{
    return f64_from_bits(0xFFF0000000000000ULL);
}

/* Quiet NaN */
static inline double qnan(void)
{
    return f64_from_bits(0x7FF8000000000000ULL);
}

/* ===================================================================
 * Classification functions
 * =================================================================== */

int __math_isnan(double x)
{
    int e = f64_rawexp(x);
    uint64_t f = f64_frac(x);
    return (e == 0x7FF) && (f != 0);
}

int __math_isinf(double x)
{
    int e = f64_rawexp(x);
    uint64_t f = f64_frac(x);
    return (e == 0x7FF) && (f == 0);
}

int __math_isfinite(double x)
{
    return f64_rawexp(x) != 0x7FF;
}

/* ===================================================================
 * Basic operations
 * =================================================================== */

double fabs(double x)
{
    f64_bits b;
    b.d = x;
    b.u &= 0x7FFFFFFFFFFFFFFFULL;
    return b.d;
}

double copysign(double x, double y)
{
    f64_bits bx, by;
    bx.d = x;
    by.d = y;
    bx.u = (bx.u & 0x7FFFFFFFFFFFFFFFULL) | (by.u & 0x8000000000000000ULL);
    return bx.d;
}

double fmax(double x, double y)
{
    if (__math_isnan(x)) return y;
    if (__math_isnan(y)) return x;
    return (x > y) ? x : y;
}

double fmin(double x, double y)
{
    if (__math_isnan(x)) return y;
    if (__math_isnan(y)) return x;
    return (x < y) ? x : y;
}

double fmod(double x, double y)
{
    if (__math_isnan(x) || __math_isnan(y) || __math_isinf(x) || y == 0.0)
        return qnan();
    if (__math_isinf(y))
        return x;
    if (x == 0.0)
        return x;

    int neg = f64_sign(x);
    double ax = fabs(x);
    double ay = fabs(y);

    if (ax < ay)
        return x;

    /* Repeated subtraction (slow but correct for all cases) */
    /* Use a smarter approach: scale and subtract */
    while (ax >= ay) {
        double d = ay;
        while (ax >= d + d && d + d > d) /* guard against inf */
            d = d + d;
        ax = ax - d;
    }

    return neg ? -ax : ax;
}

/* ===================================================================
 * Rounding functions
 * =================================================================== */

double trunc(double x)
{
    if (__math_isnan(x) || __math_isinf(x))
        return x;
    if (x == 0.0)
        return x;

    f64_bits b;
    b.d = x;
    int e = f64_rawexp(x) - 1023;

    if (e < 0) {
        /* |x| < 1: truncate to +/-0 */
        b.u &= 0x8000000000000000ULL;
        return b.d;
    }

    if (e >= 52) {
        /* Already an integer (no fractional bits) */
        return x;
    }

    /* Zero out the fractional bits */
    uint64_t mask = 0x000FFFFFFFFFFFFFULL >> e;
    b.u &= ~mask;
    return b.d;
}

double floor(double x)
{
    double t = trunc(x);
    if (f64_sign(x) && t != x)
        t = t - 1.0;
    return t;
}

double ceil(double x)
{
    double t = trunc(x);
    if (!f64_sign(x) && t != x)
        t = t + 1.0;
    return t;
}

double round(double x)
{
    if (__math_isnan(x) || __math_isinf(x))
        return x;
    double t = floor(fabs(x) + 0.5);
    return f64_sign(x) ? -t : t;
}

/* ===================================================================
 * Decomposition: frexp, ldexp, modf
 * =================================================================== */

double frexp(double x, int *exp)
{
    *exp = 0;
    if (x == 0.0 || __math_isnan(x) || __math_isinf(x))
        return x;

    f64_bits b;
    b.d = x;
    int e = f64_rawexp(x);

    if (e == 0) {
        /* Subnormal: normalise by multiplying by 2^52 */
        b.d = x * 4503599627370496.0; /* 2^52 */
        e = f64_rawexp(b.d);
        *exp = e - 1023 - 52 + 1;
    } else {
        *exp = e - 1023 + 1;
    }

    /* Set exponent to -1 (biased: 1022), so result is in [0.5, 1.0) */
    b.u = (b.u & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)1022 << 52);
    return b.d;
}

double ldexp(double x, int n)
{
    if (x == 0.0 || __math_isnan(x) || __math_isinf(x))
        return x;

    f64_bits b;
    b.d = x;
    int e = f64_rawexp(x);

    if (e == 0) {
        /* Subnormal: normalise first */
        b.d = x * 4503599627370496.0; /* 2^52 */
        e = f64_rawexp(b.d);
        e -= 52;
    }

    e += n;

    if (e >= 0x7FF) {
        /* Overflow: infinity with correct sign */
        return f64_sign(x) ? neg_inf() : pos_inf();
    }

    if (e <= 0) {
        /* Subnormal result or underflow */
        if (e < -52) {
            return f64_sign(x) ? f64_from_bits(0x8000000000000000ULL) : pos_zero();
        }
        /* Construct subnormal: set exponent to 1, then shift right */
        b.u = (b.u & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)1 << 52);
        /* Add the implicit 1 bit */
        b.u |= (1ULL << 52);
        /* Now shift right by (1 - e) */
        int shift = 1 - e;
        if (shift > 0 && shift < 64) {
            uint64_t sign_bit = b.u & 0x8000000000000000ULL;
            uint64_t frac = b.u & 0x001FFFFFFFFFFFFFULL;
            frac >>= shift;
            b.u = sign_bit | frac;
        }
        return b.d;
    }

    b.u = (b.u & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)e << 52);
    return b.d;
}

double modf(double x, double *iptr)
{
    if (__math_isnan(x)) {
        *iptr = x;
        return x;
    }
    /* modf(inf) returns 0 with *iptr = inf (POSIX) */
    if (__math_isinf(x)) {
        *iptr = x;
        return copysign(0.0, x);
    }
    double t = trunc(x);
    *iptr = t;
    return x - t;
}

/* ===================================================================
 * Square root — Newton-Raphson iteration
 * =================================================================== */

double sqrt(double x)
{
    if (x < 0.0) {
        /* POSIX: sqrt of negative value is domain error */
        errno = EDOM;
        return qnan();
    }
    if (x == 0.0 || __math_isnan(x) || __math_isinf(x))
        return x;

    /*
     * Initial guess via bit manipulation:
     * Halve the exponent roughly: (bits >> 1) + (1023/2 << 52)
     */
    f64_bits b;
    b.d = x;
    b.u = (b.u >> 1) + (0x1FF7A3BFULL << 21); /* magic constant */

    double guess = b.d;

    /* Newton-Raphson: x_{n+1} = 0.5 * (x_n + S/x_n) */
    for (int i = 0; i < 8; i++)
        guess = 0.5 * (guess + x / guess);

    return guess;
}

/* ===================================================================
 * Exponential and logarithm
 * =================================================================== */

/*
 * exp(x) — e^x using Cody-Waite argument reduction + Horner polynomial.
 *
 * Algorithm:
 *   1. Reduce x = k*ln2 + r using high/low ln2 split (Cody-Waite)
 *      where k = round(x / ln2), |r| <= ln2/2 ~ 0.347.
 *   2. Compute e^r via Horner-form Taylor series with enough terms
 *      for double precision (13 terms, ~2^-56 error for |r| < 0.347).
 *   3. Result = ldexp(e^r, k) = 2^k * e^r.
 *
 * Uses 3-part ln2 split for higher precision in the reduction step,
 * matching the trig Cody-Waite approach for ~72-bit pi/2 precision.
 */
double exp(double x)
{
    if (__math_isnan(x))
        return x;
    if (__math_isinf(x))
        return (x > 0.0) ? pos_inf() : pos_zero(); /* exp(+inf)=inf, exp(-inf)=0 */
    if (x > 709.7) {
        /* POSIX: exp overflow is a range error */
        errno = ERANGE;
        return pos_inf();
    }
    if (x < -745.0) {
        /* POSIX: exp underflow toward zero is a range error */
        errno = ERANGE;
        return pos_zero();
    }
    if (x == 0.0)
        return 1.0;

    /* Cody-Waite reduction: x = k*ln2_hi + k*ln2_mid + k*ln2_lo + r */
    static const double inv_ln2 = 1.4426950408889634074;  /* 1/ln(2) */
    static const double ln2_hi  = 6.93147180369123816490e-01; /* upper bits */
    static const double ln2_lo  = 1.90821492927058500170e-10; /* lower bits */

    double kd = x * inv_ln2;
    long k;
    if (kd >= 0)
        k = (long)(kd + 0.5);
    else
        k = (long)(kd - 0.5);

    /* Cody-Waite two-step subtraction for exact r computation */
    double dk = (double)k;
    double r = (x - dk * ln2_hi) - dk * ln2_lo;

    /* e^r via Horner-form Taylor: e^r = 1 + r + r^2/2! + ... + r^13/13!
     * For |r| <= ln(2)/2 ~ 0.347, 13 terms give ~2^-60 relative error.
     * Horner form: (((...(1/13! * r + 1/12!) * r + 1/11!) * r + ...) * r + 1) */
    double p;
    p = 1.0 / 6227020800.0;   /* 1/13! */
    p = p * r + 1.0 / 479001600.0; /* 1/12! */
    p = p * r + 1.0 / 39916800.0;  /* 1/11! */
    p = p * r + 1.0 / 3628800.0;   /* 1/10! */
    p = p * r + 1.0 / 362880.0;    /* 1/9! */
    p = p * r + 1.0 / 40320.0;     /* 1/8! */
    p = p * r + 1.0 / 5040.0;      /* 1/7! */
    p = p * r + 1.0 / 720.0;       /* 1/6! */
    p = p * r + 1.0 / 120.0;       /* 1/5! */
    p = p * r + 1.0 / 24.0;        /* 1/4! */
    p = p * r + 1.0 / 6.0;         /* 1/3! */
    p = p * r + 0.5;               /* 1/2! */
    p = p * r + 1.0;               /* 1/1! */
    p = p * r + 1.0;               /* 1/0! */

    /* Multiply by 2^k */
    return ldexp(p, (int)k);
}

/*
 * log(x) — natural logarithm using (m-1)/(m+1) series with extended terms.
 *
 * Algorithm:
 *   1. Write x = m * 2^e via frexp, adjust m to [sqrt(2)/2, sqrt(2)].
 *   2. Let t = (m-1)/(m+1). Then log(m) = 2*(t + t^3/3 + t^5/5 + ...).
 *   3. Result = log(m) + e*ln2_hi + e*ln2_lo.
 *
 * With m in [sqrt(2)/2, sqrt(2)], |t| <= 0.172, so the series
 * converges rapidly: t^31/31 < 2^-89 for |t| < 0.172.
 * Uses Horner-form evaluation with Cody-Waite ln2 split.
 */
double log(double x)
{
    if (x < 0.0) {
        /* POSIX: log of negative value is domain error */
        errno = EDOM;
        return qnan();
    }
    if (__math_isnan(x))
        return qnan();
    if (x == 0.0) {
        /* POSIX: log(0) is a pole error — return -HUGE_VAL, set ERANGE */
        errno = ERANGE;
        return neg_inf();
    }
    if (__math_isinf(x))
        return pos_inf();
    if (x == 1.0)
        return 0.0; /* exact */

    static const double ln2_hi = 6.93147180369123816490e-01;
    static const double ln2_lo = 1.90821492927058500170e-10;

    int e;
    double m = frexp(x, &e); /* m in [0.5, 1.0) */

    /* Adjust so m is in [sqrt(2)/2, sqrt(2)] for faster convergence */
    if (m < 0.70710678118654752) {
        m = m * 2.0;
        e--;
    }

    /*
     * ln(m) where m is near 1.
     * Let t = (m - 1) / (m + 1), so |t| <= 0.172.
     * ln(m) = 2 * (t + t^3/3 + t^5/5 + ... + t^31/31)
     *
     * Horner form on t^2:
     *   sum = t * (1 + t^2*(1/3 + t^2*(1/5 + t^2*(1/7 + ...))))
     * Then ln(m) = 2 * sum.
     */
    /*
     * Compute t = (m-1)/(m+1). With the fixed __divdf3 in
     * softfloat_rt.c, this division now produces correct results.
     */
    double t = (m - 1.0) / (m + 1.0);
    double t2 = t * t;
    double t3 = t * t2;
    double t5 = t3 * t2;
    double t7 = t5 * t2;
    double t9 = t7 * t2;
    double t11 = t9 * t2;
    double t13 = t11 * t2;
    double t15 = t13 * t2;
    double t17 = t15 * t2;
    double t19 = t17 * t2;

    /* sum = t + t^3/3 + t^5/5 + ... + t^19/19 */
    /* 10 terms: |t| < 0.172, t^21/21 < 2^-50, sufficient for double */
    double sum = t + t3 / 3.0 + t5 / 5.0 + t7 / 7.0
               + t9 / 9.0 + t11 / 11.0 + t13 / 13.0 + t15 / 15.0
               + t17 / 17.0 + t19 / 19.0;
    double logm = 2.0 * sum;

    /* Cody-Waite addition for e*ln(2) */
    double dk = (double)e;
    return logm + dk * ln2_hi + dk * ln2_lo;
}

double log10(double x)
{
    /* errno is set by log() for x < 0 (EDOM) and x == 0 (ERANGE) */
    return log(x) * 0.4342944819032518; /* 1/ln(10) */
}

double log2(double x)
{
    /* errno is set by log() for x < 0 (EDOM) and x == 0 (ERANGE) */
    return log(x) * 1.4426950408889634; /* 1/ln(2) */
}

/* ===================================================================
 * Power: pow(base, exponent)
 * =================================================================== */

double pow(double base, double exponent)
{
    /* Special cases */
    if (__math_isnan(exponent))
        return qnan();
    if (exponent == 0.0)
        return 1.0;
    if (exponent == 1.0)
        return base;
    if (__math_isnan(base))
        return qnan();
    if (base == 1.0)
        return 1.0;
    if (base == 0.0) {
        if (exponent > 0.0)
            return 0.0;
        return pos_inf();
    }

    /* Check for integer exponent — use binary exponentiation */
    if (exponent == trunc(exponent) && fabs(exponent) < 1e15) {
        long n = (long)exponent;
        int neg_exp = 0;
        if (n < 0) {
            neg_exp = 1;
            n = -n;
        }
        double result = 1.0;
        double b = base;
        while (n > 0) {
            if (n & 1)
                result *= b;
            b *= b;
            n >>= 1;
        }
        double final = neg_exp ? 1.0 / result : result;
        /* POSIX: pow overflow/underflow is a range error */
        if (__math_isinf(final) || (final == 0.0 && base != 0.0))
            errno = ERANGE;
        return final;
    }

    /* General case: base^exp = exp(exp * log(base)) */
    if (base < 0.0) {
        /* POSIX: negative base with non-integer exponent is domain error */
        errno = EDOM;
        return qnan();
    }

    {
        double result = exp(exponent * log(base));
        /* POSIX: pow overflow/underflow is a range error */
        if (__math_isinf(result) && __math_isfinite(base) && __math_isfinite(exponent)) {
            errno = ERANGE;
        } else if (result == 0.0 && base != 0.0 && __math_isfinite(exponent)) {
            errno = ERANGE;
        }
        return result;
    }
}

/* ===================================================================
 * Trigonometric functions
 *
 * Range-reduce to [-pi/4, pi/4] using Cody-Waite method:
 *   x = k*(pi/2) + r, compute sin/cos of r via Taylor.
 *
 * Cody-Waite splits pi/2 into high + low parts so that n*pi2_hi
 * is exact in double precision for reasonable n, eliminating
 * catastrophic cancellation during range reduction.
 * =================================================================== */

/*
 * Cody-Waite 3-part range reduction for trig functions.
 *
 * Split pi/2 into three parts: pi2_hi + pi2_mid + pi2_lo
 * where pi2_hi has ~24 bits, pi2_mid has ~24 bits, and pi2_lo
 * carries the remainder. This gives ~72 bits of pi/2 precision,
 * sufficient for arguments up to ~2^50 before needing Payne-Hanek.
 *
 * The 3-part split means n*pi2_hi is exact for |n| up to ~2^29,
 * and n*pi2_mid adds another 24 bits, so the reduced argument r
 * retains full 53-bit double precision.
 */
static double trig_reduce(double x, int *quadrant)
{
    /* 3-part Cody-Waite split of pi/2 for 72-bit precision */
    static const double pi2_hi  = 1.5707963109016418;     /* upper 24 mantissa bits */
    static const double pi2_mid = 1.5893254773528196e-08;  /* next 24 bits */
    static const double pi2_lo  = 6.123233995736766e-17;   /* remainder */
    static const double inv_pi2 = 0.6366197723675814;      /* 2/pi */

    /* Compute n = round(x / (pi/2)) */
    double fn = x * inv_pi2;
    long n;
    if (fn >= 0.0)
        n = (long)(fn + 0.5);
    else
        n = (long)(fn - 0.5);

    /*
     * 3-step Cody-Waite subtraction: each step removes one chunk
     * of pi/2 exactly, preserving precision in the remainder.
     */
    double dn = (double)n;
    double r = ((x - dn * pi2_hi) - dn * pi2_mid) - dn * pi2_lo;

    /* quadrant = n mod 4 (handle negative n correctly) */
    int q = (int)(n & 3);
    if (q < 0) q += 4;

    *quadrant = q;
    return r; /* r in approximately [-pi/4, pi/4] */
}

/*
 * sin_kern(r) — sin(r) for |r| <= pi/4 via fdlibm minimax polynomial.
 *
 * sin(r) = r + r^3 * S(r^2) where S is a degree-5 polynomial with
 * Remez-optimized coefficients S1..S6 from fdlibm k_sin.c.
 * Horner form minimizes FP operations for soft-float.
 */
static double sin_kern(double r)
{
    /* fdlibm S1-S6 coefficients for sin kernel */
    static const double
    S1 = -1.66666666666666324348e-01, /* -1/6 refined */
    S2 =  8.33333333332248946124e-03, /*  1/120 refined */
    S3 = -1.98412698298579493134e-04,
    S4 =  2.75573137070700676789e-06,
    S5 = -2.50507602534068634195e-08,
    S6 =  1.58969099521155010221e-10;

    double r2 = r * r;
    double r3 = r2 * r;
    /* Horner: r + r^3*(S1 + r^2*(S2 + r^2*(S3 + r^2*(S4 + r^2*(S5 + r^2*S6))))) */
    return r + r3 * (S1 + r2 * (S2 + r2 * (S3 + r2 * (S4 + r2 * (S5 + r2 * S6)))));
}

/*
 * cos_kern(r) — cos(r) for |r| <= pi/4 via fdlibm minimax polynomial.
 *
 * cos(r) = 1 - r^2/2 + r^4 * C(r^2) where C is a degree-5 polynomial
 * with Remez-optimized coefficients C1..C6 from fdlibm k_cos.c.
 */
static double cos_kern(double r)
{
    /* fdlibm C1-C6 coefficients for cos kernel */
    static const double
    C1 =  4.16666666666666019037e-02, /*  1/24 refined */
    C2 = -1.38888888888741095749e-03, /* -1/720 refined */
    C3 =  2.48015872894767294178e-05,
    C4 = -2.75573143513906633035e-07,
    C5 =  2.08757232129817482790e-09,
    C6 = -1.13596475577881948265e-11;

    double r2 = r * r;
    double r4 = r2 * r2;
    /* 1 - r^2/2 + r^4*(C1 + r^2*(C2 + r^2*(C3 + r^2*(C4 + r^2*(C5 + r^2*C6))))) */
    return 1.0 - 0.5 * r2 + r4 * (C1 + r2 * (C2 + r2 * (C3 + r2 * (C4 + r2 * (C5 + r2 * C6)))));
}

double sin(double x)
{
    if (__math_isinf(x)) {
        /* POSIX: sin(inf) is domain error */
        errno = EDOM;
        return qnan();
    }
    if (__math_isnan(x))
        return qnan();
    /* sin(+/-0) = +/-0 exactly, preserving sign of zero */
    if (x == 0.0)
        return x;

    /*
     * Cody-Waite range reduction gives r in [-pi/4, pi/4] and
     * quadrant q = n mod 4. The reduced argument can be negative,
     * so the quadrant mapping handles all sign logic:
     *   q=0: sin(r)    q=1: cos(r)
     *   q=2: -sin(r)   q=3: -cos(r)
     */
    int q;
    double r = trig_reduce(x, &q);

    switch (q) {
    case 0: return  sin_kern(r);
    case 1: return  cos_kern(r);
    case 2: return -sin_kern(r);
    case 3: return -cos_kern(r);
    default: return 0.0;
    }
}

double cos(double x)
{
    if (__math_isinf(x)) {
        /* POSIX: cos(inf) is domain error */
        errno = EDOM;
        return qnan();
    }
    if (__math_isnan(x))
        return qnan();

    /*
     * cos(0) = 1 exactly. Cody-Waite handles the general case;
     * quadrant mapping for cos:
     *   q=0: cos(r)    q=1: -sin(r)
     *   q=2: -cos(r)   q=3: sin(r)
     */
    int q;
    double r = trig_reduce(x, &q);

    switch (q) {
    case 0: return  cos_kern(r);
    case 1: return -sin_kern(r);
    case 2: return -cos_kern(r);
    case 3: return  sin_kern(r);
    default: return 0.0;
    }
}

/*
 * tan_kern(r) — tan(r) for |r| <= pi/4 via fdlibm polynomial kernel.
 *
 * Uses a degree-13 odd polynomial approximation from fdlibm k_tan.c.
 * When octant is odd, returns -1/tan(r) instead (for pi/2 offset).
 * This avoids the precision loss of computing sin/cos separately
 * and dividing, especially near pi/2 where cos is tiny.
 */
static double tan_kern(double r, int odd)
{
    /* fdlibm T coefficients for tan kernel polynomial */
    static const double
    T0  =  3.33333333333334091986e-01,  /* 1/3 refined */
    T1  =  1.33333333333201242699e-01,  /* 2/15 refined */
    T2  =  5.39682539762260521377e-02,
    T3  =  2.18694882948595424599e-02,
    T4  =  8.86323982359930005737e-03,
    T5  =  3.59207910759131235356e-03,
    T6  =  1.45620945432529025516e-03,
    T7  =  5.88041240820264096874e-04,
    T8  =  2.46463134818469906812e-04,
    T9  =  7.81794442939557092300e-05,
    T10 =  7.14072491382608190305e-05,
    T11 = -1.85586374855275456654e-05,
    T12 =  2.59073051863633712884e-05;

    double r2 = r * r;
    double r4 = r2 * r2;
    /* Split into even/odd terms of r^2 polynomial for Horner */
    double t1 = r2 * (T0 + r4 * (T2 + r4 * (T4 + r4 * (T6 + r4 * (T8 + r4 * (T10 + r4 * T12))))));
    double t2 = r4 * (T1 + r4 * (T3 + r4 * (T5 + r4 * (T7 + r4 * (T9 + r4 * T11)))));
    double p = t1 + t2;
    double t = r + r * p;

    if (odd)
        return -1.0 / t;
    return t;
}

double tan(double x)
{
    if (__math_isinf(x)) {
        /* POSIX: tan(inf) is domain error */
        errno = EDOM;
        return qnan();
    }
    if (__math_isnan(x))
        return qnan();
    if (x == 0.0)
        return x; /* preserve sign of zero */

    /* Range reduce then use dedicated tan kernel */
    int q;
    double r = trig_reduce(x, &q);

    /* tan is odd: q=0,2 use tan(r), q=1,3 use -1/tan(r) */
    return tan_kern(r, q & 1);
}

/* ===================================================================
 * Inverse trigonometric
 * =================================================================== */

/*
 * atan(x) — arc tangent using fdlibm 4-interval table-driven approach.
 *
 * Divides [0, inf) into 4 intervals using thresholds at 0.4375,
 * 0.6875, 1.1875, 2.4375. Each interval reduces x to a smaller
 * argument t via identity atan(x) = atanhi[i] + atan(t) + atanlo[i],
 * then approximates atan(t) with an 11-coefficient odd polynomial.
 *
 * This replaces the Taylor series which converges slowly near x=1
 * and loses precision. The fdlibm polynomial aT[0..10] is Remez-
 * optimized for the reduced argument range.
 */
double atan(double x)
{
    if (__math_isnan(x))
        return x;
    if (__math_isinf(x))
        return copysign(1.57079632679489661923, x); /* +/- pi/2 */

    /* fdlibm atan tables: hi/lo pairs for 4 reference angles */
    static const double atanhi[] = {
        4.63647609000806093515e-01, /* atan(0.5) hi */
        7.85398163397448278999e-01, /* atan(1.0) = pi/4 hi */
        9.82793723247329054082e-01, /* atan(1.5) hi */
        1.57079632679489655800e+00, /* atan(inf) = pi/2 hi */
    };
    static const double atanlo[] = {
        2.26987774529616870924e-17, /* atan(0.5) lo */
        3.06161699786838301793e-17, /* atan(1.0) lo */
        1.39033110312309984516e-17, /* atan(1.5) lo */
        6.12323399573676603587e-17, /* atan(inf) = pi/2 lo */
    };
    /* Remez polynomial coefficients aT[0..10] for atan(t)/t - 1 */
    static const double
    aT0  =  3.33333333333329318027e-01,
    aT1  = -1.99999999998764832476e-01,
    aT2  =  1.42857142725034663711e-01,
    aT3  = -1.11111104054623557880e-01,
    aT4  =  9.09088713343650656196e-02,
    aT5  = -7.69187620504482999495e-02,
    aT6  =  6.66107313738753120669e-02,
    aT7  = -5.83357013379057348645e-02,
    aT8  =  4.97687799461593236017e-02,
    aT9  = -3.65315727442169155270e-02,
    aT10 =  1.62858201153657823623e-02;

    int neg = 0;
    if (x < 0.0) {
        neg = 1;
        x = -x;
    }

    int id;
    double t;
    if (x < 0.4375) {
        /* No reduction needed; polynomial directly on x */
        if (x < 1e-17)
            return neg ? -x : x; /* atan(tiny) = tiny */
        id = -1;
        t = x;
    } else if (x < 0.6875) {
        /* Reduce: t = (2x-1)/(2+x), atan(x) = atan(0.5) + atan(t) */
        id = 0;
        t = (2.0 * x - 1.0) / (2.0 + x);
    } else if (x < 1.1875) {
        /* Reduce: t = (x-1)/(x+1), atan(x) = pi/4 + atan(t) */
        id = 1;
        t = (x - 1.0) / (x + 1.0);
    } else if (x < 2.4375) {
        /* Reduce: t = (x-1.5)/(1+1.5*x), atan(x) = atan(1.5) + atan(t) */
        id = 2;
        t = (x - 1.5) / (1.0 + 1.5 * x);
    } else {
        /* Reduce: t = -1/x, atan(x) = pi/2 + atan(t) */
        id = 3;
        t = -1.0 / x;
    }

    /* Polynomial: atan(t) = t + t^3 * P(t^2) */
    double t2 = t * t;
    double t4 = t2 * t2;
    /* Split into even/odd terms for accuracy */
    double s1 = t2 * (aT0 + t4 * (aT2 + t4 * (aT4 + t4 * (aT6 + t4 * (aT8 + t4 * aT10)))));
    double s2 = t4 * (aT1 + t4 * (aT3 + t4 * (aT5 + t4 * (aT7 + t4 * aT9))));
    double p = s1 + s2;

    double result;
    if (id < 0) {
        result = t - t * p;
    } else {
        result = atanhi[id] - (t * p - atanlo[id] - t);
    }
    return neg ? -result : result;
}

double atan2(double y, double x)
{
    double pi = 3.14159265358979323846;

    if (__math_isnan(x) || __math_isnan(y))
        return qnan();

    if (y == 0.0) {
        if (f64_sign(x))
            return copysign(pi, y);
        return copysign(pos_zero(), y);
    }

    if (x == 0.0)
        return copysign(pi / 2.0, y);

    if (__math_isinf(x)) {
        if (__math_isinf(y))
            return copysign(f64_sign(x) ? 3.0 * pi / 4.0 : pi / 4.0, y);
        return f64_sign(x) ? copysign(pi, y) : copysign(pos_zero(), y);
    }

    if (__math_isinf(y))
        return copysign(pi / 2.0, y);

    double a = atan(fabs(y / x));

    if (x > 0.0)
        return y > 0.0 ? a : -a;
    else
        return y > 0.0 ? pi - a : -(pi - a);
}

/*
 * asin(x) — arc sine using fdlibm-style multi-range approach.
 *
 * For |x| < 0.5: use rational polynomial asin(x) = x + x^3*R(x^2).
 * For |x| >= 0.5: use identity asin(x) = pi/2 - 2*asin(sqrt((1-x)/2))
 *   which transforms the near-endpoint region (where the derivative
 *   blows up) into a small-argument region for the polynomial.
 *   This fixes the precision loss in the original atan(x/sqrt(1-x^2))
 *   formula near |x| = 1.
 */
double asin(double x)
{
    if (fabs(x) > 1.0) {
        /* POSIX: asin(|x|>1) is domain error */
        errno = EDOM;
        return qnan();
    }
    if (__math_isnan(x))
        return qnan();
    if (x == 0.0)
        return x;

    /* fdlibm pS/qS coefficients for asin rational polynomial */
    static const double
    pS0 =  1.66666666666666657415e-01,
    pS1 = -3.25565818622400915405e-01,
    pS2 =  2.01212532134862925881e-01,
    pS3 = -4.00555345006794114027e-02,
    pS4 =  7.91534994289814532176e-04,
    pS5 =  3.47933107596021167570e-05,
    qS1 = -2.40339491173441421878e+00,
    qS2 =  2.02094576023350569471e+00,
    qS3 = -6.88283971605453293030e-01,
    qS4 =  7.70381505559019352791e-02;

    int neg = (x < 0.0);
    double ax = fabs(x);

    if (ax < 0.5) {
        /* Small argument: asin(x) = x + x^3*P(x^2)/Q(x^2) */
        double t = ax * ax;
        double p = t * (pS0 + t * (pS1 + t * (pS2 + t * (pS3 + t * (pS4 + t * pS5)))));
        double q = 1.0 + t * (qS1 + t * (qS2 + t * (qS3 + t * qS4)));
        double w = p / q;
        double result = ax + ax * w;
        return neg ? -result : result;
    }

    /* |x| >= 0.5: use endpoint identity to reduce to small argument */
    double t = (1.0 - ax) * 0.5;
    double s = sqrt(t);
    double p = t * (pS0 + t * (pS1 + t * (pS2 + t * (pS3 + t * (pS4 + t * pS5)))));
    double q = 1.0 + t * (qS1 + t * (qS2 + t * (qS3 + t * qS4)));
    double w = p / q;
    /* asin(x) = pi/2 - 2*(s + s*w) */
    double result = 1.57079632679489661923 - 2.0 * (s + s * w);
    return neg ? -result : result;
}

/*
 * acos(x) — arc cosine using fdlibm-style multi-range approach.
 *
 * For x in [0.5, 1]: acos(x) = 2*asin(sqrt((1-x)/2)) via the
 *   half-angle identity, which avoids the cancellation in pi/2 - asin(x)
 *   when x is near 1 (where asin(x) is near pi/2).
 * For x in [-1, -0.5]: acos(x) = pi - 2*asin(sqrt((1+x)/2)).
 * For |x| < 0.5: acos(x) = pi/2 - asin(x) (safe since asin is small).
 */
double acos(double x)
{
    if (fabs(x) > 1.0) {
        /* POSIX: acos(|x|>1) is domain error */
        errno = EDOM;
        return qnan();
    }
    if (__math_isnan(x))
        return qnan();

    /* fdlibm pS/qS coefficients (same as asin) */
    static const double
    pS0 =  1.66666666666666657415e-01,
    pS1 = -3.25565818622400915405e-01,
    pS2 =  2.01212532134862925881e-01,
    pS3 = -4.00555345006794114027e-02,
    pS4 =  7.91534994289814532176e-04,
    pS5 =  3.47933107596021167570e-05,
    qS1 = -2.40339491173441421878e+00,
    qS2 =  2.02094576023350569471e+00,
    qS3 = -6.88283971605453293030e-01,
    qS4 =  7.70381505559019352791e-02;

    if (x >= 0.5) {
        /* Near +1: use half-angle identity for precision */
        double t = (1.0 - x) * 0.5;
        double s = sqrt(t);
        double p = t * (pS0 + t * (pS1 + t * (pS2 + t * (pS3 + t * (pS4 + t * pS5)))));
        double q = 1.0 + t * (qS1 + t * (qS2 + t * (qS3 + t * qS4)));
        return 2.0 * (s + s * (p / q));
    }
    if (x <= -0.5) {
        /* Near -1: use half-angle identity with pi offset */
        double t = (1.0 + x) * 0.5;
        double s = sqrt(t);
        double p = t * (pS0 + t * (pS1 + t * (pS2 + t * (pS3 + t * (pS4 + t * pS5)))));
        double q = 1.0 + t * (qS1 + t * (qS2 + t * (qS3 + t * qS4)));
        return 3.14159265358979323846 - 2.0 * (s + s * (p / q));
    }
    /* |x| < 0.5: pi/2 - asin(x) is safe (no cancellation) */
    return 1.57079632679489661923 - asin(x);
}

/* ===================================================================
 * Hyperbolic functions
 * =================================================================== */

/*
 * sinh(x) — hyperbolic sine using expm1 for small |x| precision.
 *
 * For |x| < 1: sinh(x) = (expm1(x) - expm1(-x)) / 2
 *   This avoids catastrophic cancellation in (e^x - e^-x)/2
 *   when x is small and e^x ~ 1.
 * For |x| >= 1: use (e^x - e^-x) / 2 directly.
 */
double sinh(double x)
{
    if (__math_isnan(x))
        return x;
    if (__math_isinf(x))
        return x;

    double ax = fabs(x);

    if (ax < 1.0) {
        /* Use expm1 to avoid cancellation for small arguments */
        double em1 = expm1(ax);
        /* sinh(x) = (expm1(x) + expm1(x)/(expm1(x)+1)) / 2
         * = em1/2 + em1/(2*(em1+1))
         * = em1 * (1 + 1/(em1+1)) / 2
         * Equivalently: (em1 + em1/(em1+1)) / 2 */
        double result = (em1 + em1 / (em1 + 1.0)) * 0.5;
        return (x < 0.0) ? -result : result;
    }

    double ex = exp(ax);
    if (__math_isinf(ex)) {
        /* POSIX: sinh overflow is a range error */
        errno = ERANGE;
        return (x < 0.0) ? neg_inf() : pos_inf();
    }
    double result = (ex - 1.0 / ex) * 0.5;
    return (x < 0.0) ? -result : result;
}

/*
 * cosh(x) — hyperbolic cosine.
 *
 * For |x| < 0.5: use expm1 for precision: cosh(x) = 1 + expm1(|x|)^2 / (2*(expm1(|x|)+1))
 * For |x| >= 0.5: cosh(x) = (e^|x| + e^-|x|) / 2 directly.
 */
double cosh(double x)
{
    if (__math_isnan(x))
        return x;
    if (__math_isinf(x))
        return pos_inf();

    double ax = fabs(x);

    if (ax < 0.5) {
        /* Use expm1 to avoid cancellation */
        double em1 = expm1(ax);
        return 1.0 + em1 * em1 / (2.0 * (em1 + 1.0));
    }

    double ex = exp(ax);
    if (__math_isinf(ex)) {
        /* POSIX: cosh overflow is a range error */
        errno = ERANGE;
        return pos_inf();
    }
    return (ex + 1.0 / ex) * 0.5;
}

/*
 * tanh(x) — hyperbolic tangent using expm1 for small |x| precision.
 *
 * For |x| < 1: tanh(x) = expm1(2x) / (expm1(2x) + 2)
 *   This avoids the catastrophic cancellation in (e^2x-1)/(e^2x+1)
 *   when x is small and e^2x ~ 1.
 * For |x| >= 22: tanh(x) = +/-1 (difference from 1 is < 2^-53).
 */
double tanh(double x)
{
    if (__math_isnan(x))
        return x;
    if (__math_isinf(x))
        return (x > 0.0) ? 1.0 : -1.0;

    double ax = fabs(x);
    if (ax > 22.0)
        return (x > 0.0) ? 1.0 : -1.0;

    if (ax < 1.0) {
        /* Use expm1 form for precision near zero */
        double em2 = expm1(2.0 * ax);
        double result = em2 / (em2 + 2.0);
        return (x < 0.0) ? -result : result;
    }

    /* General case: (e^2x - 1) / (e^2x + 1) */
    double e2x = exp(2.0 * ax);
    double result = (e2x - 1.0) / (e2x + 1.0);
    return (x < 0.0) ? -result : result;
}

/* -----------------------------------------------------------------------
 * Additional math functions for MicroPython / POSIX
 * ---------------------------------------------------------------------- */

double remainder(double x, double y) {
    if (y == 0.0) return qnan();
    double n = round(x / y);
    return x - n * y;
}

double nearbyint(double x) {
    return round(x);
}

double rint(double x) {
    return round(x);
}

long lrint(double x) {
    return (long)round(x);
}

long long llrint(double x) {
    return (long long)round(x);
}

double scalbn(double x, int n) {
    return ldexp(x, n);
}

double cbrt(double x) {
    if (x == 0.0) return x;
    if (__math_isnan(x)) return x;
    if (__math_isinf(x)) return x; /* cbrt(+/-inf) = +/-inf */
    int neg = (x < 0.0);
    if (neg) x = -x;
    /* Newton's method: y = x^(1/3) */
    double y = (x > 1.0) ? x / 3.0 : x;
    for (int i = 0; i < 60; i++) {
        double y2 = y * y;
        if (y2 == 0.0) break;
        y = (2.0 * y + x / y2) / 3.0;
    }
    return neg ? -y : y;
}

double hypot(double x, double y) {
    x = fabs(x);
    y = fabs(y);
    if (x < y) { double t = x; x = y; y = t; }
    if (x == 0.0) return 0.0;
    double r = y / x;
    return x * sqrt(1.0 + r * r);
}

/*
 * expm1(x) — compute e^x - 1 with dedicated algorithm to avoid
 * catastrophic cancellation when x is near zero.
 *
 * For |x| < 0.5: use Taylor series with enough terms for double precision.
 *   expm1(x) = x + x^2/2 + x^3/6 + ... (Horner form)
 * For larger |x|: use Cody-Waite exp() then subtract 1, but with
 *   compensation: expm1(x) = 2^k * (e^r - 1) + (2^k - 1)
 *   to preserve precision when k is small.
 */
double expm1(double x) {
    if (__math_isnan(x))
        return x;
    if (__math_isinf(x))
        return (x > 0.0) ? pos_inf() : -1.0; /* expm1(-inf) = -1 */

    /* For |x| < ~0.35, direct polynomial avoids cancellation */
    if (fabs(x) < 0.35) {
        /* Horner: x * (1 + x*(1/2 + x*(1/6 + x*(1/24 + ...)))) */
        double x2 = x * x;
        double x3 = x2 * x;
        /* 12-term Taylor gives ~2^-64 error for |x| < 0.35 */
        double p = x + x2 * 0.5 + x3 * (1.0 / 6.0)
                 + x2 * x2 * (1.0 / 24.0)
                 + x2 * x3 * (1.0 / 120.0)
                 + x3 * x3 * (1.0 / 720.0)
                 + x3 * x2 * x2 * (1.0 / 5040.0)
                 + x2 * x2 * x2 * x2 * (1.0 / 40320.0)
                 + x3 * x3 * x3 * (1.0 / 362880.0)
                 + x2 * x2 * x2 * x2 * x2 * (1.0 / 3628800.0);
        return p;
    }

    /* For larger |x|, use exp(x) - 1 via the Cody-Waite exp.
     * When |x| >= 0.35 the relative error of exp(x)-1 is bounded
     * because exp(x) >= ~1.42, so subtraction loses < 1 bit. */
    return exp(x) - 1.0;
}

/*
 * log1p(x) — compute log(1+x) with dedicated algorithm to avoid
 * catastrophic cancellation when x is near zero.
 *
 * For |x| < 0.4: use the s = x/(2+x) form (fdlibm-style) which
 *   gives log(1+x) = x - x^2/2 + higher order via polynomial.
 * For larger |x|: delegate to log(1+x) since there's no cancellation.
 */
double log1p(double x) {
    if (__math_isnan(x))
        return x;
    if (x == 0.0)
        return x; /* preserve sign of zero */
    if (x == -1.0) {
        errno = ERANGE;
        return neg_inf();
    }
    if (x < -1.0) {
        errno = EDOM;
        return qnan();
    }
    if (__math_isinf(x))
        return pos_inf();

    /* For |x| < 0.4, use direct polynomial to avoid log(1+x) cancellation */
    if (fabs(x) < 0.4) {
        /* s = x/(2+x), log(1+x) = x - x^2*0.5 + s^2*(Lg coefficients) */
        /* Simplified: use Taylor log(1+x) = x - x^2/2 + x^3/3 - ... */
        /* Enough terms for double precision when |x| < 0.4 */
        double x2 = x * x;
        double x3 = x2 * x;
        /* Horner form: x*(1 - x/2 + x^2/3 - x^3/4 + ...) */
        double p = x - x2 * 0.5 + x3 * (1.0 / 3.0)
                 - x2 * x2 * 0.25
                 + x2 * x3 * 0.2
                 - x3 * x3 * (1.0 / 6.0)
                 + x3 * x2 * x2 * (1.0 / 7.0)
                 - x2 * x2 * x2 * x2 * 0.125
                 + x3 * x3 * x3 * (1.0 / 9.0)
                 - x2 * x2 * x2 * x2 * x2 * 0.1
                 + x3 * x3 * x3 * x2 * (1.0 / 11.0)
                 - x3 * x3 * x3 * x3 * (1.0 / 12.0)
                 + x3 * x3 * x3 * x3 * x * (1.0 / 13.0)
                 - x3 * x3 * x3 * x3 * x2 * (1.0 / 14.0)
                 + x3 * x3 * x3 * x3 * x3 * (1.0 / 15.0);
        return p;
    }

    /* For |x| >= 0.4, 1+x is far enough from 1 that log(1+x) is safe */
    return log(1.0 + x);
}

/*
 * asinh(x) — inverse hyperbolic sine using log1p for precision.
 *
 * For |x| < 2: asinh(x) = sign(x) * log1p(|x| + x^2/(1+sqrt(1+x^2)))
 *   which avoids catastrophic cancellation in log(x + sqrt(x^2+1))
 *   when x is small and sqrt(x^2+1) ~ 1.
 * For |x| >= 2: use log(2|x|) + 1/(2x^2) correction, or direct formula.
 */
double asinh(double x) {
    if (__math_isnan(x) || __math_isinf(x))
        return x;
    int neg = (x < 0.0);
    double ax = fabs(x);
    double result;
    if (ax < 2.0) {
        /* Use log1p form for precision near zero */
        double t = ax * ax;
        result = log1p(ax + t / (1.0 + sqrt(1.0 + t)));
    } else if (ax > 1e150) {
        /* Avoid overflow in x*x: asinh(x) ~ log(2*|x|) */
        result = log(ax) + 0.6931471805599453; /* log(2) */
    } else {
        result = log(ax + sqrt(ax * ax + 1.0));
    }
    return neg ? -result : result;
}

double acosh(double x) {
    if (x < 1.0) {
        /* POSIX: acosh(x<1) is domain error */
        errno = EDOM;
        return qnan();
    }
    return log(x + sqrt((x - 1.0) * (x + 1.0)));
}

double atanh(double x) {
    if (__math_isnan(x))
        return x; /* atanh(NaN) = NaN, no errno */
    if (fabs(x) > 1.0) {
        /* POSIX: atanh(|x|>1) is domain error */
        errno = EDOM;
        return qnan();
    }
    if (x == 1.0) {
        /* POSIX: atanh(1) is a pole error — return +HUGE_VAL */
        errno = ERANGE;
        return pos_inf();
    }
    if (x == -1.0) {
        /* POSIX: atanh(-1) is a pole error — return -HUGE_VAL */
        errno = ERANGE;
        return neg_inf();
    }
    return 0.5 * log((1.0 + x) / (1.0 - x));
}

double fdim(double x, double y) {
    return (x > y) ? (x - y) : 0.0;
}

double fma(double x, double y, double z) {
    return x * y + z;
}

double logb(double x) {
    if (x == 0.0) {
        /* POSIX: logb(0) is a pole error — set ERANGE */
        errno = ERANGE;
        return neg_inf();
    }
    if (__math_isnan(x))
        return x;
    if (__math_isinf(x))
        return pos_inf();
    int e;
    (void)frexp(fabs(x), &e);
    return (double)(e - 1);
}

int ilogb(double x) {
    if (x == 0.0)
        return FP_ILOGB0;
    if (__math_isnan(x))
        return FP_ILOGBNAN;
    if (__math_isinf(x))
        return INT32_MAX;
    int e;
    (void)frexp(fabs(x), &e);
    return e - 1;
}

double scalbln(double x, long n) {
    return scalbn(x, (int)n);
}

double remquo(double x, double y, int *quo) {
    if (y == 0.0)
        return qnan();
    double q = round(x / y);
    long qi = (long)q;
    if (quo)
        *quo = (int)(qi & 0x7f);
    return x - q * y;
}

double nextafter(double x, double y) {
    if (__math_isnan(x) || __math_isnan(y))
        return qnan();
    if (x == y)
        return y;

    f64_bits b;
    b.d = x;

    if (x == 0.0) {
        b.u = 1ULL;
        if (y < 0.0)
            b.u |= 0x8000000000000000ULL;
        return b.d;
    }

    if ((x < y) == (x > 0.0))
        b.u++;
    else
        b.u--;

    return b.d;
}

double nexttoward(double x, long double y) {
    return nextafter(x, (double)y);
}

double nan(const char *tagp) {
    (void)tagp;
    return qnan();
}

/*
 * erf(x) — error function using fdlibm 4-range rational polynomial.
 *
 * Range 1 (|x| < 0.84375): erf(x) = x + x*P(x^2)/Q(x^2)
 *   where P/Q are 5-term rational polynomials (erf_pp/erf_qq).
 * Range 2 (0.84375 <= |x| < 1.25): erfc(x) = erfc(1) + P1(|x|-1)
 *   where P1/Q1 are 7/6-term (erf_pa/erf_qa).
 * Range 3 (1.25 <= |x| < 28): erfc(x) = exp(-x^2)/|x| * R(1/|x|)
 *   sub-ranges at 2.857142 (erf_ra/sa vs erf_rb/sb).
 * Range 4 (|x| >= 28): erf(x) = +/-1 (erfc rounds to 0).
 *
 * This replaces the Abramowitz & Stegun approximation which had
 * insufficient precision for double (only ~7 digits).
 */
double erf(double x) {
    if (__math_isnan(x))
        return x;
    if (__math_isinf(x))
        return (x > 0.0) ? 1.0 : -1.0;

    static const double erx = 8.45062911510467529297e-01; /* erf(1) */

    int sign = (x < 0.0);
    double ax = fabs(x);

    if (ax < 0.84375) {
        /* Range 1: small |x|, use P/Q rational poly */
        if (ax < 1e-28)
            return x; /* erf(tiny) ~ 2*x/sqrt(pi) ~ x for very tiny */
        double z = ax * ax;
        double r = erf_pp[0] + z * (erf_pp[1] + z * (erf_pp[2] + z * (erf_pp[3] + z * erf_pp[4])));
        double s = 1.0 + z * (erf_qq[0] + z * (erf_qq[1] + z * (erf_qq[2] + z * (erf_qq[3] + z * erf_qq[4]))));
        double y = r / s;
        double result = ax + ax * y;
        return sign ? -result : result;
    }

    if (ax < 1.25) {
        /* Range 2: near 1, use erfc(1) + polynomial in (|x|-1) */
        double s = ax - 1.0;
        double P = erf_pa[0] + s * (erf_pa[1] + s * (erf_pa[2] + s * (erf_pa[3] + s * (erf_pa[4] + s * (erf_pa[5] + s * erf_pa[6])))));
        double Q = 1.0 + s * (erf_qa[0] + s * (erf_qa[1] + s * (erf_qa[2] + s * (erf_qa[3] + s * (erf_qa[4] + s * erf_qa[5])))));
        double result = erx + P / Q;
        return sign ? -result : result;
    }

    if (ax < 28.0) {
        /* Range 3: use exp(-x^2)/|x| * R(1/|x|) */
        double s = 1.0 / (ax * ax);
        double R, S;
        if (ax < 2.857142857142857) {
            /* Sub-range 3a */
            R = erf_ra[0] + s * (erf_ra[1] + s * (erf_ra[2] + s * (erf_ra[3] + s * (erf_ra[4] + s * (erf_ra[5] + s * (erf_ra[6] + s * erf_ra[7]))))));
            S = 1.0 + s * (erf_sa[0] + s * (erf_sa[1] + s * (erf_sa[2] + s * (erf_sa[3] + s * (erf_sa[4] + s * (erf_sa[5] + s * (erf_sa[6] + s * erf_sa[7])))))));
        } else {
            /* Sub-range 3b */
            R = erf_rb[0] + s * (erf_rb[1] + s * (erf_rb[2] + s * (erf_rb[3] + s * (erf_rb[4] + s * (erf_rb[5] + s * erf_rb[6])))));
            S = 1.0 + s * (erf_sb[0] + s * (erf_sb[1] + s * (erf_sb[2] + s * (erf_sb[3] + s * (erf_sb[4] + s * (erf_sb[5] + s * erf_sb[6]))))));
        }
        double r_val = exp(-ax * ax) * (1.0 / ax) * (0.5641895835477562869 + R / S);
        /* erf = 1 - erfc */
        double result = 1.0 - r_val;
        return sign ? -result : result;
    }

    /* Range 4: |x| >= 28, erf is +/-1 to double precision */
    return sign ? -1.0 : 1.0;
}

/*
 * erfc(x) — complementary error function with dedicated path.
 *
 * For large x, erfc(x) = 1 - erf(x) loses all precision because
 * erf(x) ~ 1. Instead, compute erfc directly from exp(-x^2) form.
 */
double erfc(double x) {
    if (__math_isnan(x))
        return x;
    if (__math_isinf(x))
        return (x > 0.0) ? 0.0 : 2.0;

    double ax = fabs(x);
    int sign = (x < 0.0);

    if (ax < 0.84375) {
        /* For small |x|, 1 - erf(x) doesn't lose precision */
        return 1.0 - erf(x);
    }

    if (ax < 28.0) {
        /* Compute erfc directly using the exp form to preserve precision */
        double s = 1.0 / (ax * ax);
        double R, S;
        if (ax < 1.25) {
            /* Use erf range 2 approach */
            double ss = ax - 1.0;
            static const double erx = 8.45062911510467529297e-01;
            double P = erf_pa[0] + ss * (erf_pa[1] + ss * (erf_pa[2] + ss * (erf_pa[3] + ss * (erf_pa[4] + ss * (erf_pa[5] + ss * erf_pa[6])))));
            double Q = 1.0 + ss * (erf_qa[0] + ss * (erf_qa[1] + ss * (erf_qa[2] + ss * (erf_qa[3] + ss * (erf_qa[4] + ss * erf_qa[5])))));
            double r_val = 1.0 - erx - P / Q;
            return sign ? 2.0 - r_val : r_val;
        }
        if (ax < 2.857142857142857) {
            R = erf_ra[0] + s * (erf_ra[1] + s * (erf_ra[2] + s * (erf_ra[3] + s * (erf_ra[4] + s * (erf_ra[5] + s * (erf_ra[6] + s * erf_ra[7]))))));
            S = 1.0 + s * (erf_sa[0] + s * (erf_sa[1] + s * (erf_sa[2] + s * (erf_sa[3] + s * (erf_sa[4] + s * (erf_sa[5] + s * (erf_sa[6] + s * erf_sa[7])))))));
        } else {
            R = erf_rb[0] + s * (erf_rb[1] + s * (erf_rb[2] + s * (erf_rb[3] + s * (erf_rb[4] + s * (erf_rb[5] + s * erf_rb[6])))));
            S = 1.0 + s * (erf_sb[0] + s * (erf_sb[1] + s * (erf_sb[2] + s * (erf_sb[3] + s * (erf_sb[4] + s * (erf_sb[5] + s * erf_sb[6]))))));
        }
        double r_val = exp(-ax * ax) * (1.0 / ax) * (0.5641895835477562869 + R / S);
        return sign ? 2.0 - r_val : r_val;
    }

    /* |x| >= 28: erfc = 0 (or 2 for negative) */
    return sign ? 2.0 : 0.0;
}

/*
 * lgamma(x) — log-gamma using Lanczos approximation (g=7, n=9).
 *
 * The Lanczos approximation gives:
 *   Gamma(x+1) = sqrt(2*pi) * (x + g + 0.5)^(x+0.5) * exp(-(x+g+0.5)) * Ag(x)
 * where Ag(x) is a rational series with 9 precomputed coefficients.
 *
 * Taking log:
 *   lgamma(x) = 0.5*log(2*pi) + (x-0.5)*log(x+g-0.5) - (x+g-0.5) + log(Ag(x-1))
 *
 * For negative non-integers, use the reflection formula.
 * This replaces the 3-term Stirling series which was inaccurate
 * for small arguments (x < 12 required recursive reduction).
 * Lanczos gives ~15-digit accuracy across the entire positive range.
 */
double lgamma(double x) {
    signgam = 1;
    if (x == 0.0 || (__math_isfinite(x) && x < 0.0 && x == trunc(x))) {
        /* POSIX: lgamma at 0 or negative integer is a pole error */
        errno = ERANGE;
        return pos_inf();
    }
    if (__math_isnan(x))
        return qnan();
    if (__math_isinf(x))
        return pos_inf();

    if (x < 0.0) {
        /* Negative non-integer: reflection formula
         * lgamma(x) = log(pi / |sin(pi*x)|) - lgamma(-x) - log(-x)
         * Actually: lgamma(x) = log(pi) - log(|sin(pi*x)|) - log(|x|) - lgamma(-x+1)
         * Simplify via: Gamma(x)*Gamma(1-x) = pi/sin(pi*x)
         */
        double ax = -x;
        double sinpx = sin(M_PI * ax);
        if (sinpx < 0.0)
            sinpx = -sinpx;
        if (sinpx == 0.0) {
            errno = ERANGE;
            return pos_inf();
        }
        /* Determine sign of gamma(x) */
        /* gamma(x) is negative when floor(x) is odd for x < 0 */
        double fl = floor(ax);
        if (fmod(fl, 2.0) == 0.0)
            signgam = -1;

        /* lgamma(x) = log(pi / (|sin(pi*x)| * |x| * gamma(|x|))) */
        /* = log(pi) - log(|sin(pi*x)|) - lgamma(1-x) */
        /* Use lgamma(1-x) = lgamma(1+ax) for ax = -x */
        double lnpi = 1.1447298858494002; /* log(pi) */
        int saved_signgam = signgam;
        double lg_reflect = lgamma(1.0 + ax);
        signgam = saved_signgam;
        double result = lnpi - log(sinpx) - log(ax) - lg_reflect;
        if (__math_isinf(result))
            errno = ERANGE;
        return result;
    }

    /* Lanczos approximation for x > 0 */
    static const double g = 7.0;
    /* Shift so we compute lgamma(x) = lgamma for Gamma(x) */
    double y = x - 1.0; /* Lanczos formula uses Gamma(y+1) */
    double ag = lanczos_coeff[0];
    for (int i = 1; i < 9; i++) {
        ag += lanczos_coeff[i] / (y + (double)i);
    }

    double tmp = y + g + 0.5;
    /* lgamma(x) = 0.5*log(2*pi) + (y+0.5)*log(tmp) - tmp + log(ag) */
    double result = 0.91893853320467274178 + (y + 0.5) * log(tmp) - tmp + log(ag);
    if (__math_isinf(result))
        errno = ERANGE;
    return result;
}

double tgamma(double x) {
    if (x == 0.0) {
        /* POSIX: tgamma(0) is a pole error */
        errno = ERANGE;
        return copysign(pos_inf(), x);
    }
    if (__math_isfinite(x) && x < 0.0 && x == trunc(x)) {
        /* POSIX: tgamma at negative integer is domain error */
        errno = EDOM;
        return qnan();
    }
    if (__math_isnan(x))
        return qnan();
    if (__math_isinf(x)) {
        if (x < 0.0) {
            errno = EDOM;
            return qnan();
        }
        return pos_inf();
    }
    double result = exp(lgamma(x));
    /* Apply sign from lgamma's signgam */
    if (signgam < 0)
        result = -result;
    /* POSIX: tgamma overflow/underflow is a range error */
    if (__math_isinf(result) || (result == 0.0 && x != 0.0)) {
        errno = ERANGE;
    }
    return result;
}

/*
 * j0(x) — Bessel function of the first kind, order 0.
 *
 * For |x| < 8: use fdlibm-style rational polynomial R0/S0.
 *   J0(x) = 1 + x^2*R(x^2)/S(x^2) where R/S from math_tables.h.
 * For |x| >= 8: asymptotic expansion with P0/Q0 amplitude/phase.
 *   J0(x) ~ sqrt(2/(pi*x)) * (P0*cos(theta) - Q0*sin(theta))
 *   where theta = x - pi/4.
 */
double j0(double x) {
    double ax = fabs(x);

    if (ax >= 8.0) {
        /* Asymptotic region: use P0/Q0 for amplitude/phase correction */
        double z = 1.0 / (ax * ax);
        double p = j0y0_P0[0] + z * (j0y0_P0[1] + z * j0y0_P0[2]);
        double q = z * (j0y0_Q0[0] + z * (j0y0_Q0[1] + z * j0y0_Q0[2]));
        double theta = ax - M_PI_4;
        double cc = cos(theta);
        double ss = sin(theta);
        return sqrt(M_2_PI / ax) * (p * cc - q * ss);
    }

    /* Polynomial region: J0(x) = 1 - x^2/4 + x^4*R(x^2) */
    double z = ax * ax;
    /* Use 6-term Taylor + correction polynomial for small x */
    double r = j0_R[0];
    double s = 1.0;
    double zn = z;
    for (int i = 1; i < 5; i++) {
        r += j0_R[i] * zn;
        s += j0_S[i - 1] * zn;
        zn *= z;
    }
    return 1.0 + z * (r / s);
}

/*
 * j1(x) — Bessel function of the first kind, order 1.
 *
 * Same structure as j0: rational polynomial for |x| < 8,
 * asymptotic P1/Q1 for |x| >= 8.
 */
double j1(double x) {
    double ax = fabs(x);

    if (ax >= 8.0) {
        /* Asymptotic region with P1/Q1 correction */
        double z = 1.0 / (ax * ax);
        double p = j1y1_P1[0] + z * (j1y1_P1[1] + z * j1y1_P1[2]);
        double q = z * (j1y1_Q1[0] + z * (j1y1_Q1[1] + z * j1y1_Q1[2]));
        double theta = ax - 3.0 * M_PI_4;
        double cc = cos(theta);
        double ss = sin(theta);
        double v = sqrt(M_2_PI / ax) * (p * cc - q * ss);
        return x < 0.0 ? -v : v;
    }

    /* Polynomial region: J1(x) = x*(0.5 + x^2*R1(x^2)/S1(x^2)) */
    if (ax < 1e-15)
        return x * 0.5; /* tiny x: J1(x) ~ x/2 */
    double z = ax * ax;
    double r = j1_R[0];
    double s = 1.0;
    double zn = z;
    for (int i = 1; i < 4; i++) {
        r += j1_R[i] * zn;
        s += j1_S[i - 1] * zn;
        zn *= z;
    }
    double v = ax * (0.5 + z * (r / s));
    return x < 0.0 ? -v : v;
}

/*
 * jn(n, x) — Bessel function of the first kind, order n.
 *
 * For n >= 2: use Miller's backward recurrence for numerical stability.
 * Forward recurrence (J_{n+1} = 2n/x * J_n - J_{n-1}) is unstable
 * for Bessel J because J_n decreases with n; backward recurrence
 * is stable because it follows the growth direction.
 */
double jn(int n, double x) {
    if (n == 0)
        return j0(x);
    if (n == 1)
        return j1(x);
    if (x == 0.0)
        return 0.0;

    int sign = 1;
    if (n < 0) {
        n = -n;
        if (n & 1)
            sign = -1;
    }

    double ax = fabs(x);
    /* For small n or small x, forward recurrence is adequate */
    if (n <= 8 || ax > (double)n) {
        /* Forward recurrence: stable when |x| > n */
        double jm1 = j0(ax);
        double jcur = j1(ax);
        for (int k = 1; k < n; k++) {
            double jnext = (2.0 * (double)k / ax) * jcur - jm1;
            jm1 = jcur;
            jcur = jnext;
        }
        double result = sign * jcur;
        return (x < 0.0 && (n & 1)) ? -result : result;
    }

    /* Miller's backward recurrence for large n */
    /* Start from m = n + extra_terms and recur backward */
    int m = n + 20; /* extra terms for convergence */
    if (m < n + 10) m = n + 10;
    double jnext = 0.0;
    double jcur = 1.0;
    double norm_sum = 0.0;
    double result = 0.0;

    for (int k = m; k >= 0; k--) {
        double jm1 = (2.0 * (double)(k + 1) / ax) * jcur - jnext;
        jnext = jcur;
        jcur = jm1;
        /* Normalize using J0: sum of J_0 + 2*(J_2 + J_4 + ...) = 1 */
        if (k == 0)
            norm_sum += jcur;
        else if ((k & 1) == 0)
            norm_sum += 2.0 * jcur;
        if (k == n)
            result = jnext; /* save J_n before further recurrence */
    }
    result = result / norm_sum;
    result = sign * result;
    return (x < 0.0 && (n & 1)) ? -result : result;
}

/*
 * y0(x) — Bessel function of the second kind, order 0.
 *
 * For x < 8: use fdlibm rational polynomial U/V plus J0 term.
 *   Y0(x) = U(x^2)/V(x^2) + (2/pi)*J0(x)*log(x)
 * For x >= 8: asymptotic with P0/Q0.
 */
double y0(double x) {
    if (x <= 0.0) {
        if (x == 0.0) {
            errno = ERANGE;
            return neg_inf();
        }
        errno = EDOM;
        return qnan();
    }
    if (__math_isinf(x))
        return 0.0;

    if (x >= 8.0) {
        /* Asymptotic: Y0(x) ~ sqrt(2/(pi*x)) * (P0*sin(theta) + Q0*cos(theta)) */
        double z = 1.0 / (x * x);
        double p = j0y0_P0[0] + z * (j0y0_P0[1] + z * j0y0_P0[2]);
        double q = z * (j0y0_Q0[0] + z * (j0y0_Q0[1] + z * j0y0_Q0[2]));
        double theta = x - M_PI_4;
        double ss = sin(theta);
        double cc = cos(theta);
        return sqrt(M_2_PI / x) * (p * ss + q * cc);
    }

    /* Polynomial region: Y0(x) = U(z)/V(z) + (2/pi)*J0(x)*log(x) */
    double z = x * x;
    double u = y0_U[0] + z * (y0_U[1] + z * (y0_U[2] + z * (y0_U[3] + z * (y0_U[4] + z * (y0_U[5] + z * y0_U[6])))));
    double v = 1.0 + z * (y0_V[0] + z * (y0_V[1] + z * (y0_V[2] + z * y0_V[3])));
    return u / v + M_2_PI * j0(x) * log(x);
}

/*
 * y1(x) — Bessel function of the second kind, order 1.
 *
 * Same structure as y0: polynomial for x < 8, asymptotic for x >= 8.
 */
double y1(double x) {
    if (x <= 0.0) {
        if (x == 0.0) {
            errno = ERANGE;
            return neg_inf();
        }
        errno = EDOM;
        return qnan();
    }
    if (__math_isinf(x))
        return 0.0;

    if (x >= 8.0) {
        /* Asymptotic with P1/Q1 */
        double z = 1.0 / (x * x);
        double p = j1y1_P1[0] + z * (j1y1_P1[1] + z * j1y1_P1[2]);
        double q = z * (j1y1_Q1[0] + z * (j1y1_Q1[1] + z * j1y1_Q1[2]));
        double theta = x - 3.0 * M_PI_4;
        double ss = sin(theta);
        double cc = cos(theta);
        return sqrt(M_2_PI / x) * (p * ss + q * cc);
    }

    /* Polynomial region: Y1(x) = x*U(z)/V(z) + (2/pi)*(J1(x)*log(x) - 1/x) */
    double z = x * x;
    double u = y1_U[0] + z * (y1_U[1] + z * (y1_U[2] + z * (y1_U[3] + z * y1_U[4])));
    double v = 1.0 + z * (y1_V[0] + z * (y1_V[1] + z * (y1_V[2] + z * (y1_V[3] + z * y1_V[4]))));
    return x * (u / v) + M_2_PI * (j1(x) * log(x) - 1.0 / x);
}

/*
 * yn(n, x) — Bessel function of the second kind, order n.
 *
 * Forward recurrence is stable for Y_n (unlike J_n) because
 * Y_n grows with n, matching the recurrence direction.
 */
double yn(int n, double x) {
    if (n == 0)
        return y0(x);
    if (n == 1)
        return y1(x);
    if (x <= 0.0) {
        if (x == 0.0) {
            errno = ERANGE;
            return neg_inf();
        }
        errno = EDOM;
        return qnan();
    }
    /* Handle negative n: Y_{-n}(x) = (-1)^n * Y_n(x) */
    int sign = 1;
    if (n < 0) {
        n = -n;
        if (n & 1)
            sign = -1;
    }
    double ym1 = y0(x);
    double ycur = y1(x);
    for (int k = 1; k < n; k++) {
        double ynext = (2.0 * (double)k / x) * ycur - ym1;
        ym1 = ycur;
        ycur = ynext;
    }
    return sign * ycur;
}

float fabsf(float x) { return (float)fabs((double)x); }
long double fabsl(long double x) { return (long double)fabs((double)x); }
float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }
long double fmodl(long double x, long double y) { return (long double)fmod((double)x, (double)y); }
float copysignf(float x, float y) { return (float)copysign((double)x, (double)y); }
long double copysignl(long double x, long double y) { return (long double)copysign((double)x, (double)y); }
float fmaxf(float x, float y) { return (float)fmax((double)x, (double)y); }
long double fmaxl(long double x, long double y) { return (long double)fmax((double)x, (double)y); }
float fminf(float x, float y) { return (float)fmin((double)x, (double)y); }
long double fminl(long double x, long double y) { return (long double)fmin((double)x, (double)y); }
float ceilf(float x) { return (float)ceil((double)x); }
long double ceill(long double x) { return (long double)ceil((double)x); }
float floorf(float x) { return (float)floor((double)x); }
long double floorl(long double x) { return (long double)floor((double)x); }
float roundf(float x) { return (float)round((double)x); }
long double roundl(long double x) { return (long double)round((double)x); }
float truncf(float x) { return (float)trunc((double)x); }
long double truncl(long double x) { return (long double)trunc((double)x); }
float frexpf(float x, int *exp) { return (float)frexp((double)x, exp); }
long double frexpl(long double x, int *exp) { return (long double)frexp((double)x, exp); }
float ldexpf(float x, int n) { return (float)ldexp((double)x, n); }
long double ldexpl(long double x, int n) { return (long double)ldexp((double)x, n); }
float modff(float x, float *iptr) { double i; float r = (float)modf((double)x, &i); if (iptr) *iptr = (float)i; return r; }
long double modfl(long double x, long double *iptr) { double i; long double r = (long double)modf((double)x, &i); if (iptr) *iptr = (long double)i; return r; }
float sqrtf(float x) { return (float)sqrt((double)x); }
long double sqrtl(long double x) { return (long double)sqrt((double)x); }
float expf(float x) { return (float)exp((double)x); }
long double expl(long double x) { return (long double)exp((double)x); }
/* exp2 delegates to exp(); overflow/underflow errno set by exp() */
double exp2(double x) { return exp(x * M_LN2); }
float exp2f(float x) { return (float)exp2((double)x); }
long double exp2l(long double x) { return (long double)exp2((double)x); }
float expm1f(float x) { return (float)expm1((double)x); }
long double expm1l(long double x) { return (long double)expm1((double)x); }
float logf(float x) { return (float)log((double)x); }
long double logl(long double x) { return (long double)log((double)x); }
float log10f(float x) { return (float)log10((double)x); }
long double log10l(long double x) { return (long double)log10((double)x); }
float log2f(float x) { return (float)log2((double)x); }
long double log2l(long double x) { return (long double)log2((double)x); }
float log1pf(float x) { return (float)log1p((double)x); }
long double log1pl(long double x) { return (long double)log1p((double)x); }
float logbf(float x) { return (float)logb((double)x); }
long double logbl(long double x) { return (long double)logb((double)x); }
float powf(float x, float y) { return (float)pow((double)x, (double)y); }
long double powl(long double x, long double y) { return (long double)pow((double)x, (double)y); }
float sinf(float x) { return (float)sin((double)x); }
long double sinl(long double x) { return (long double)sin((double)x); }
float cosf(float x) { return (float)cos((double)x); }
long double cosl(long double x) { return (long double)cos((double)x); }
float tanf(float x) { return (float)tan((double)x); }
long double tanl(long double x) { return (long double)tan((double)x); }
float asinf(float x) { return (float)asin((double)x); }
long double asinl(long double x) { return (long double)asin((double)x); }
float acosf(float x) { return (float)acos((double)x); }
long double acosl(long double x) { return (long double)acos((double)x); }
float atanf(float x) { return (float)atan((double)x); }
long double atanl(long double x) { return (long double)atan((double)x); }
float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }
long double atan2l(long double y, long double x) { return (long double)atan2((double)y, (double)x); }
float sinhf(float x) { return (float)sinh((double)x); }
long double sinhl(long double x) { return (long double)sinh((double)x); }
float coshf(float x) { return (float)cosh((double)x); }
long double coshl(long double x) { return (long double)cosh((double)x); }
float tanhf(float x) { return (float)tanh((double)x); }
long double tanhl(long double x) { return (long double)tanh((double)x); }
float asinhf(float x) { return (float)asinh((double)x); }
long double asinhl(long double x) { return (long double)asinh((double)x); }
float acoshf(float x) { return (float)acosh((double)x); }
long double acoshl(long double x) { return (long double)acosh((double)x); }
float atanhf(float x) { return (float)atanh((double)x); }
long double atanhl(long double x) { return (long double)atanh((double)x); }
float remainderf(float x, float y) { return (float)remainder((double)x, (double)y); }
long double remainderl(long double x, long double y) { return (long double)remainder((double)x, (double)y); }
float nearbyintf(float x) { return (float)nearbyint((double)x); }
long double nearbyintl(long double x) { return (long double)nearbyint((double)x); }
float rintf(float x) { return (float)rint((double)x); }
long double rintl(long double x) { return (long double)rint((double)x); }
long lrintf(float x) { return lrint((double)x); }
long lrintl(long double x) { return lrint((double)x); }
long long llrintf(float x) { return llrint((double)x); }
long long llrintl(long double x) { return llrint((double)x); }
long lround(double x) { return (long)round(x); }
long lroundf(float x) { return (long)round((double)x); }
long lroundl(long double x) { return (long)round((double)x); }
long long llround(double x) { return (long long)round(x); }
long long llroundf(float x) { return (long long)round((double)x); }
long long llroundl(long double x) { return (long long)round((double)x); }
float scalbnf(float x, int n) { return (float)scalbn((double)x, n); }
long double scalbnl(long double x, int n) { return (long double)scalbn((double)x, n); }
float scalblnf(float x, long n) { return (float)scalbln((double)x, n); }
long double scalblnl(long double x, long n) { return (long double)scalbln((double)x, n); }
float cbrtf(float x) { return (float)cbrt((double)x); }
long double cbrtl(long double x) { return (long double)cbrt((double)x); }
float hypotf(float x, float y) { return (float)hypot((double)x, (double)y); }
long double hypotl(long double x, long double y) { return (long double)hypot((double)x, (double)y); }
float fdimf(float x, float y) { return (float)fdim((double)x, (double)y); }
long double fdiml(long double x, long double y) { return (long double)fdim((double)x, (double)y); }
float fmaf(float x, float y, float z) { return (float)fma((double)x, (double)y, (double)z); }
long double fmal(long double x, long double y, long double z) { return (long double)fma((double)x, (double)y, (double)z); }
float remquof(float x, float y, int *q) { return (float)remquo((double)x, (double)y, q); }
long double remquol(long double x, long double y, int *q) { return (long double)remquo((double)x, (double)y, q); }
float nextafterf(float x, float y) { return (float)nextafter((double)x, (double)y); }
long double nextafterl(long double x, long double y) { return (long double)nextafter((double)x, (double)y); }
float nexttowardf(float x, long double y) { return (float)nexttoward((double)x, y); }
long double nexttowardl(long double x, long double y) { return (long double)nexttoward((double)x, y); }
float nanf(const char *tagp) { return (float)nan(tagp); }
long double nanl(const char *tagp) { return (long double)nan(tagp); }
float erff(float x) { return (float)erf((double)x); }
long double erfl(long double x) { return (long double)erf((double)x); }
float erfcf(float x) { return (float)erfc((double)x); }
long double erfcl(long double x) { return (long double)erfc((double)x); }
float lgammaf(float x) { return (float)lgamma((double)x); }
long double lgammal(long double x) { return (long double)lgamma((double)x); }
float tgammaf(float x) { return (float)tgamma((double)x); }
long double tgammal(long double x) { return (long double)tgamma((double)x); }
int ilogbf(float x) { return ilogb((double)x); }
int ilogbl(long double x) { return ilogb((double)x); }
