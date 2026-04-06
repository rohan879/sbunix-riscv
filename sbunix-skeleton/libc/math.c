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
 */

#include <math.h>
#include <stdint.h>
#include <errno.h>  /* errno / EDOM / ERANGE — needed for math_errhandling == MATH_ERRNO */

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
    double t = trunc(x);
    *iptr = t;
    if (__math_isnan(x)) {
        return x;
    }
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
 * exp(x) — e^x using the identity:
 *   Write x = k * ln(2) + r, where k = round(x / ln(2)).
 *   Then e^x = 2^k * e^r, where |r| <= ln(2)/2.
 *   Compute e^r via a minimax polynomial.
 */
double exp(double x)
{
    if (__math_isnan(x))
        return x;
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

    /* k = round(x / ln2) */
    double inv_ln2 = 1.4426950408889634; /* 1/ln(2) */
    double ln2_hi  = 0.6931471805599453;
    double ln2_lo  = 2.3190468138462996e-17;

    double kd = x * inv_ln2;
    long k;
    if (kd >= 0)
        k = (long)(kd + 0.5);
    else
        k = (long)(kd - 0.5);

    double r = x - (double)k * ln2_hi - (double)k * ln2_lo;

    /* e^r via Horner form of Taylor series (13 terms for ~double precision) */
    double r2 = r * r;
    double p;
    p = 1.0 / 3628800.0;       /* 1/10! */
    p = p * r + 1.0 / 362880.0; /* 1/9! */
    p = p * r + 1.0 / 40320.0;  /* 1/8! */
    p = p * r + 1.0 / 5040.0;   /* 1/7! */
    p = p * r + 1.0 / 720.0;    /* 1/6! */
    p = p * r + 1.0 / 120.0;    /* 1/5! */
    p = p * r + 1.0 / 24.0;     /* 1/4! */
    p = p * r + 1.0 / 6.0;      /* 1/3! */
    p = p * r + 0.5;            /* 1/2! */
    p = p * r + 1.0;            /* 1/1! */
    p = p * r + 1.0;            /* 1/0! */
    (void)r2;

    /* Multiply by 2^k */
    return ldexp(p, (int)k);
}

/*
 * log(x) — natural logarithm.
 *   Write x = m * 2^e, where 0.5 <= m < 1.0 (via frexp).
 *   Then ln(x) = e*ln(2) + ln(m).
 *   Let t = (m - 1)/(m + 1), then ln(m) = 2*(t + t^3/3 + t^5/5 + ...).
 *
 *   For better convergence, use m in [sqrt(2)/2, sqrt(2)] range.
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

    int e;
    double m = frexp(x, &e); /* m in [0.5, 1.0), x = m * 2^e */

    /* Adjust so m is in [sqrt(2)/2, sqrt(2)] for faster convergence */
    if (m < 0.70710678118654752) { /* sqrt(2)/2 */
        m = m * 2.0;
        e--;
    }

    /*
     * ln(m) where m is near 1.
     * Let t = (m - 1) / (m + 1).
     * ln(m) = 2 * (t + t^3/3 + t^5/5 + t^7/7 + ... )
     */
    double t = (m - 1.0) / (m + 1.0);
    double t2 = t * t;
    double sum = t;
    double term = t;

    for (int i = 3; i <= 31; i += 2) {
        term *= t2;
        sum += term / (double)i;
    }
    sum *= 2.0;

    return sum + (double)e * 0.6931471805599453; /* e * ln(2) */
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
 * Cody-Waite range reduction for trig functions.
 *
 * Split pi/2 into pi2_hi + pi2_lo where pi2_hi has only 30
 * significant mantissa bits, so n*pi2_hi is exact in double for
 * |n| up to ~2^22. The remainder is computed as:
 *   r = (x - n*pi2_hi) - n*pi2_lo
 * This avoids the catastrophic cancellation that occurs when
 * subtracting n*pi/2 from x using a single-precision constant.
 */
static double trig_reduce(double x, int *quadrant)
{
    /* pi/2 = pi2_hi + pi2_lo, split for Cody-Waite exact subtraction */
    static const double pi2_hi  = 1.5707963267341256;   /* 30 sig. mantissa bits */
    static const double pi2_lo  = 6.077100506506192e-11; /* remainder of pi/2 */
    static const double inv_pi2 = 0.6366197723675814;    /* 2/pi */

    /* Compute n = round(x / (pi/2)) */
    double fn = x * inv_pi2;
    long n;
    if (fn >= 0.0)
        n = (long)(fn + 0.5);
    else
        n = (long)(fn - 0.5);

    /*
     * Cody-Waite two-step subtraction: the first subtraction is exact
     * because n*pi2_hi fits in a double without rounding, leaving only
     * the small pi2_lo correction in the second step.
     */
    double r = (x - (double)n * pi2_hi) - (double)n * pi2_lo;

    /* quadrant = n mod 4 (handle negative n correctly via bitmask) */
    int q = (int)(n & 3);
    if (q < 0) q += 4;

    *quadrant = q;
    return r; /* r in approximately [-pi/4, pi/4] */
}

/*
 * sin(r) for r in [-pi/4, pi/4] via Taylor series.
 * 12 iterative terms (up to r^25) are more than sufficient for
 * double precision since |r| <= pi/4 ~ 0.785.
 */
static double sin_kern(double r)
{
    double r2 = r * r;
    double term = r;
    double sum = r;
    for (int i = 1; i <= 12; i++) {
        term *= -r2 / (double)(2 * i * (2 * i + 1));
        sum += term;
    }
    return sum;
}

/*
 * cos(r) for r in [-pi/4, pi/4] via Taylor series.
 * 12 iterative terms (up to r^24) are more than sufficient for
 * double precision since |r| <= pi/4 ~ 0.785.
 */
static double cos_kern(double r)
{
    double r2 = r * r;
    double term = 1.0;
    double sum = 1.0;
    for (int i = 1; i <= 12; i++) {
        term *= -r2 / (double)((2 * i - 1) * 2 * i);
        sum += term;
    }
    return sum;
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

double tan(double x)
{
    if (__math_isinf(x)) {
        /* POSIX: tan(inf) is domain error */
        errno = EDOM;
        return qnan();
    }
    if (__math_isnan(x))
        return qnan();
    double c = cos(x);
    if (c == 0.0)
        return copysign(pos_inf(), sin(x));
    return sin(x) / c;
}

/* ===================================================================
 * Inverse trigonometric
 * =================================================================== */

/*
 * atan(x) — arc tangent.
 *
 * For |x| <= 0.4375, use Taylor series directly (fast convergence).
 * For 0.4375 < |x| <= 1.0, use identity: atan(x) = pi/6 + atan((x*sqrt3-1)/(sqrt3+x))
 *   which reduces the argument to |t| < 0.27.
 * For |x| > 1, use atan(x) = pi/2 - atan(1/x).
 *
 * This three-range approach avoids slow convergence of the Taylor
 * series near x=1, which was causing precision loss.
 */
double atan(double x)
{
    if (__math_isnan(x))
        return x;
    if (__math_isinf(x))
        return copysign(1.57079632679489661923, x); /* +/- pi/2 */

    int neg = 0;
    if (x < 0.0) {
        neg = 1;
        x = -x;
    }

    double offset = 0.0;

    if (x > 1.0) {
        /* atan(x) = pi/2 - atan(1/x) for |x| > 1 */
        offset = 1.57079632679489661923; /* pi/2 */
        x = 1.0 / x;
        /* Now x is in (0, 1). We'll subtract the atan result from offset. */
        /* Compute atan(x) for the reduced argument and negate before adding offset */

        /* Further reduce if needed */
        if (x > 0.4375) {
            /* Use sqrt(3) reduction: atan(x) = pi/6 + atan((x*sqrt3-1)/(sqrt3+x)) */
            double sqrt3 = 1.7320508075688772;
            double t = (x * sqrt3 - 1.0) / (sqrt3 + x);
            x = t;
            offset -= 0.52359877559829887308; /* subtract pi/6 */
        }

        /* Taylor series for reduced x (now |x| <= ~0.44) */
        double x2 = x * x;
        double term = x;
        double sum = x;
        for (int i = 1; i <= 22; i++) {
            term *= -x2;
            sum += term / (double)(2 * i + 1);
        }

        double result = offset - sum;
        return neg ? -result : result;
    }

    if (x > 0.4375) {
        /* Use sqrt(3) reduction: atan(x) = pi/6 + atan((x*sqrt3-1)/(sqrt3+x)) */
        double sqrt3 = 1.7320508075688772;
        double t = (x * sqrt3 - 1.0) / (sqrt3 + x);
        x = t;
        offset = 0.52359877559829887308; /* pi/6 */
    }

    /* Taylor series for |x| <= 0.4375 (or reduced argument) */
    double x2 = x * x;
    double term = x;
    double sum = x;
    for (int i = 1; i <= 22; i++) {
        term *= -x2;
        sum += term / (double)(2 * i + 1);
    }

    double result = offset + sum;
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
    /* asin(x) = atan(x / sqrt(1 - x^2)) */
    return atan(x / sqrt(1.0 - x * x));
}

double acos(double x)
{
    if (fabs(x) > 1.0) {
        /* POSIX: acos(|x|>1) is domain error */
        errno = EDOM;
        return qnan();
    }
    if (__math_isnan(x))
        return qnan();
    /* acos(x) = pi/2 - asin(x) */
    return 1.57079632679489661923 - asin(x);
}

/* ===================================================================
 * Hyperbolic functions
 * =================================================================== */

double sinh(double x)
{
    if (__math_isnan(x))
        return x;
    if (fabs(x) < 1e-10)
        return x; /* sinh(x) ~ x for small x */

    /* sinh(x) = (e^x - e^-x) / 2 */
    double ex = exp(x);
    if (__math_isinf(ex)) {
        /* POSIX: sinh overflow is a range error */
        errno = ERANGE;
        return ex;
    }
    return (ex - 1.0 / ex) * 0.5;
}

double cosh(double x)
{
    if (__math_isnan(x))
        return x;

    /* cosh(x) = (e^x + e^-x) / 2 */
    double ex = exp(fabs(x));
    if (__math_isinf(ex)) {
        /* POSIX: cosh overflow is a range error */
        errno = ERANGE;
        return ex;
    }
    return (ex + 1.0 / ex) * 0.5;
}

double tanh(double x)
{
    if (__math_isnan(x))
        return x;
    if (x > 20.0)
        return 1.0;
    if (x < -20.0)
        return -1.0;

    /* tanh(x) = (e^2x - 1) / (e^2x + 1) */
    double e2x = exp(2.0 * x);
    return (e2x - 1.0) / (e2x + 1.0);
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

double expm1(double x) {
    if (fabs(x) < 1e-5)
        return x + 0.5 * x * x + x * x * x / 6.0;
    return exp(x) - 1.0;
}

double log1p(double x) {
    if (fabs(x) < 1e-5)
        return x - 0.5 * x * x + x * x * x / 3.0;
    return log(1.0 + x);
}

double asinh(double x) {
    return log(x + sqrt(x * x + 1.0));
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
    if (x == 0.0)
        return -pos_inf();
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

double erf(double x) {
    if (__math_isnan(x))
        return x;
    int sign = x < 0.0 ? -1 : 1;
    x = fabs(x);

    double t = 1.0 / (1.0 + 0.5 * x);
    double poly = -1.26551223 +
                  t * (1.00002368 +
                  t * (0.37409196 +
                  t * (0.09678418 +
                  t * (-0.18628806 +
                  t * (0.27886807 +
                  t * (-1.13520398 +
                  t * (1.48851587 +
                  t * (-0.82215223 +
                  t * 0.17087277))))))));
    double tau = t * exp(-x * x + poly);
    return sign > 0 ? 1.0 - tau : tau - 1.0;
}

double erfc(double x) {
    return 1.0 - erf(x);
}

double lgamma(double x) {
    signgam = 1;
    if (x == 0.0 || (__math_isfinite(x) && x < 0.0 && x == trunc(x))) {
        /* POSIX: lgamma at 0 or negative integer is a pole error (ERANGE, not EDOM) */
        errno = ERANGE;
        return pos_inf();
    }
    if (x <= 0.0 && !__math_isnan(x) && !__math_isinf(x)) {
        /*
         * Negative non-integer: use reflection formula
         *   lgamma(x) = log(pi / |x * sin(pi*x)|) - lgamma(1 - x)
         * For simplicity, compute via the positive side.
         */
        double ax = -x;
        double sinpx = sin(M_PI * x);
        if (sinpx < 0.0) {
            sinpx = -sinpx;
            signgam = -1;
        }
        if (sinpx == 0.0) {
            errno = EDOM;
            return pos_inf();
        }
        double lnpi = 1.1447298858494002; /* log(pi) */
        double result = lnpi - log(ax * sinpx) - lgamma(1.0 - x);
        /* POSIX: lgamma can overflow for large results */
        if (__math_isinf(result)) {
            errno = ERANGE;
        }
        return result;
    }
    if (__math_isnan(x))
        return qnan();
    if (__math_isinf(x))
        return pos_inf();
    if (x < 12.0) {
        double y = x;
        double p = 0.0;
        while (y < 12.0) {
            p -= log(y);
            y += 1.0;
        }
        x = y;
        /* Inline the Stirling approximation instead of recursing */
        double inv = 1.0 / x;
        double inv2 = inv * inv;
        double series = inv / 12.0 - inv2 * inv / 360.0 + inv2 * inv2 * inv / 1260.0;
        double result = p + (x - 0.5) * log(x) - x + 0.91893853320467274178 + series;
        if (__math_isinf(result)) {
            /* POSIX: lgamma overflow is a range error */
            errno = ERANGE;
        }
        return result;
    }
    double inv = 1.0 / x;
    double inv2 = inv * inv;
    double series = inv / 12.0 - inv2 * inv / 360.0 + inv2 * inv2 * inv / 1260.0;
    double result = (x - 0.5) * log(x) - x + 0.91893853320467274178 + series;
    if (__math_isinf(result)) {
        /* POSIX: lgamma overflow is a range error */
        errno = ERANGE;
    }
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

double j0(double x) {
    double ax = fabs(x);
    if (ax < 1e-4) {
        double x2 = x * x;
        return 1.0 - x2 / 4.0 + (x2 * x2) / 64.0;
    }
    return sqrt(2.0 / (M_PI * ax)) * cos(ax - M_PI_4);
}

double j1(double x) {
    double ax = fabs(x);
    if (ax < 1e-4) {
        double x2 = x * x;
        return x * (0.5 - x2 / 16.0 + (x2 * x2) / 384.0);
    }
    double v = sqrt(2.0 / (M_PI * ax)) * cos(ax - 3.0 * M_PI_4);
    return x < 0.0 ? -v : v;
}

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
    double jm1 = j0(x);
    double jcur = j1(x);
    for (int k = 1; k < n; k++) {
        double jn1 = (2.0 * k / x) * jcur - jm1;
        jm1 = jcur;
        jcur = jn1;
    }
    return sign * jcur;
}

double y0(double x) {
    if (x <= 0.0)
        return qnan();
    return sqrt(2.0 / (M_PI * x)) * sin(x - M_PI_4);
}

double y1(double x) {
    if (x <= 0.0)
        return qnan();
    return sqrt(2.0 / (M_PI * x)) * sin(x - 3.0 * M_PI_4);
}

double yn(int n, double x) {
    if (n == 0)
        return y0(x);
    if (n == 1)
        return y1(x);
    if (x <= 0.0)
        return qnan();
    double ym1 = y0(x);
    double ycur = y1(x);
    for (int k = 1; k < n; k++) {
        double yn1 = (2.0 * k / x) * ycur - ym1;
        ym1 = ycur;
        ycur = yn1;
    }
    return ycur;
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
