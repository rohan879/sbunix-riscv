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
    if (x < 0.0)
        return qnan();
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
    if (x > 709.7)
        return pos_inf();
    if (x < -745.0)
        return pos_zero();

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
    if (__math_isnan(x) || x < 0.0)
        return qnan();
    if (x == 0.0)
        return neg_inf();
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
    return log(x) * 0.4342944819032518; /* 1/ln(10) */
}

double log2(double x)
{
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
        return neg_exp ? 1.0 / result : result;
    }

    /* General case: base^exp = exp(exp * log(base)) */
    if (base < 0.0) {
        /* Negative base with non-integer exponent: NaN */
        return qnan();
    }

    return exp(exponent * log(base));
}

/* ===================================================================
 * Trigonometric functions
 *
 * Range-reduce to [-pi/4, pi/4] using the identity:
 *   x = k*(pi/2) + r, compute sin/cos of r via Taylor.
 * =================================================================== */

/* Reduce x to [-pi, pi] first, then to a quadrant */
static double trig_reduce(double x, int *quadrant)
{
    /* Handle very large values by reducing modulo 2*pi */
    double two_pi   = 6.28318530717958647693;
    double half_pi  = 1.57079632679489661923;

    /* Reduce to [0, 2*pi) */
    if (x < 0.0) {
        double n = ceil(-x / two_pi);
        x += n * two_pi;
    }
    if (x >= two_pi) {
        double n = floor(x / two_pi);
        x -= n * two_pi;
    }

    /* Now x is in [0, 2*pi). Determine quadrant. */
    int q = 0;
    if (x >= 0.0 && x < half_pi) {
        q = 0;
    } else if (x >= half_pi && x < 2.0 * half_pi) {
        q = 1;
        x -= half_pi;
    } else if (x >= 2.0 * half_pi && x < 3.0 * half_pi) {
        q = 2;
        x -= 2.0 * half_pi;
    } else {
        q = 3;
        x -= 3.0 * half_pi;
    }

    *quadrant = q;
    return x; /* r in [0, pi/2) */
}

/* sin(r) for r in [0, pi/2] via Taylor series */
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

/* cos(r) for r in [0, pi/2] via Taylor series */
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
    if (__math_isnan(x) || __math_isinf(x))
        return qnan();
    if (x == 0.0)
        return x;

    int neg = 0;
    if (x < 0.0) {
        neg = 1;
        x = -x;
    }

    int q;
    double r = trig_reduce(x, &q);
    double result;

    switch (q) {
    case 0: result =  sin_kern(r); break;
    case 1: result =  cos_kern(r); break;
    case 2: result = -sin_kern(r); break;
    case 3: result = -cos_kern(r); break;
    default: result = 0.0; break;
    }

    return neg ? -result : result;
}

double cos(double x)
{
    if (__math_isnan(x) || __math_isinf(x))
        return qnan();

    if (x < 0.0)
        x = -x;

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
 * Uses the Taylor series for |x| <= 1:
 *   atan(x) = x - x^3/3 + x^5/5 - ...
 * For |x| > 1, use atan(x) = pi/2 - atan(1/x).
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

    int invert = 0;
    if (x > 1.0) {
        invert = 1;
        x = 1.0 / x;
    }

    /* For moderate x, use identity: atan(x) = 2*atan(x/(1+sqrt(1+x^2)))
     * or just a long Taylor series. We use Taylor with enough terms. */
    double x2 = x * x;
    double term = x;
    double sum = x;
    for (int i = 1; i <= 30; i++) {
        term *= -x2;
        sum += term / (double)(2 * i + 1);
    }

    double result = sum;
    if (invert)
        result = 1.57079632679489661923 - result; /* pi/2 - atan(1/x) */

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
    if (__math_isnan(x) || fabs(x) > 1.0)
        return qnan();
    if (x == 0.0)
        return x;
    /* asin(x) = atan(x / sqrt(1 - x^2)) */
    return atan(x / sqrt(1.0 - x * x));
}

double acos(double x)
{
    if (__math_isnan(x) || fabs(x) > 1.0)
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
    if (__math_isinf(ex))
        return ex; /* overflow */
    return (ex - 1.0 / ex) * 0.5;
}

double cosh(double x)
{
    if (__math_isnan(x))
        return x;

    /* cosh(x) = (e^x + e^-x) / 2 */
    double ex = exp(fabs(x));
    if (__math_isinf(ex))
        return ex;
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
