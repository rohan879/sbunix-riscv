/*
 * math.c — minimal soft-float math library for SBUnix.
 *
 * On rv64imac (no F/D extensions), GCC emits calls to soft-float
 * routines (__adddf3, __muldf3, etc.) from libgcc for all double/float
 * arithmetic.  We write normal C code with doubles and let the compiler
 * handle the rest.
 *
 * The implementations below prioritize correctness and simplicity over
 * last-ULP accuracy.
 */

#include <math.h>
#include <stdint.h>

/* PI constant */
#define M_PI    3.14159265358979323846
#define M_PI_2  1.57079632679489661923
#define M_LN2   0.69314718055994530942
#define M_LN10  2.30258509299404568402
#define M_LOG2E 1.44269504088896340736

/* -----------------------------------------------------------------------
 * Basic functions
 * ---------------------------------------------------------------------- */

double fabs(double x) {
    return x < 0.0 ? -x : x;
}

float fabsf(float x) {
    return x < 0.0f ? -x : x;
}

double copysign(double x, double y) {
    double ax = fabs(x);
    return y < 0.0 ? -ax : ax;
}

float copysignf(float x, float y) {
    float ax = fabsf(x);
    return y < 0.0f ? -ax : ax;
}

double floor(double x) {
    if (isnan(x) || isinf(x)) return x;
    long i = (long)x;
    double d = (double)i;
    if (d == x || x >= 0.0) return d;
    return d - 1.0;
}

float floorf(float x) {
    return (float)floor((double)x);
}

double ceil(double x) {
    if (isnan(x) || isinf(x)) return x;
    long i = (long)x;
    double d = (double)i;
    if (d == x || x <= 0.0) return d;
    return d + 1.0;
}

float ceilf(float x) {
    return (float)ceil((double)x);
}

double trunc(double x) {
    if (isnan(x) || isinf(x)) return x;
    return (double)(long)x;
}

float truncf(float x) {
    return (float)trunc((double)x);
}

double round(double x) {
    if (isnan(x) || isinf(x)) return x;
    if (x >= 0.0)
        return floor(x + 0.5);
    else
        return ceil(x - 0.5);
}

float roundf(float x) {
    return (float)round((double)x);
}

double fmod(double x, double y) {
    if (y == 0.0) return NAN;
    if (isnan(x) || isnan(y)) return NAN;
    if (isinf(x)) return NAN;
    if (isinf(y)) return x;

    double q = trunc(x / y);
    return x - q * y;
}

float fmodf(float x, float y) {
    return (float)fmod((double)x, (double)y);
}

double modf(double x, double *iptr) {
    double i = trunc(x);
    *iptr = i;
    return x - i;
}

double scalbn(double x, int n) {
    /* x * 2^n */
    while (n > 0) { x *= 2.0; n--; }
    while (n < 0) { x *= 0.5; n++; }
    return x;
}

double ldexp(double x, int exp) {
    return scalbn(x, exp);
}

double frexp(double x, int *exp) {
    if (x == 0.0) { *exp = 0; return 0.0; }
    if (isnan(x) || isinf(x)) { *exp = 0; return x; }

    int e = 0;
    double ax = fabs(x);

    /* Normalize to [0.5, 1.0) */
    while (ax >= 1.0) { ax *= 0.5; e++; }
    while (ax < 0.5)  { ax *= 2.0; e--; }

    *exp = e;
    return x < 0.0 ? -ax : ax;
}

/* -----------------------------------------------------------------------
 * sqrt — Newton's method
 * ---------------------------------------------------------------------- */

double sqrt(double x) {
    if (x < 0.0) return NAN;
    if (x == 0.0 || isnan(x) || isinf(x)) return x;

    /* Initial guess using integer bit manipulation */
    double guess = x;
    if (x > 1.0)
        guess = x / 2.0;
    else
        guess = x;

    /* Newton iterations: x_{n+1} = (x_n + S/x_n) / 2 */
    for (int i = 0; i < 60; i++) {
        double next = (guess + x / guess) * 0.5;
        if (fabs(next - guess) < 1e-15 * fabs(next))
            break;
        guess = next;
    }

    return guess;
}

float sqrtf(float x) {
    return (float)sqrt((double)x);
}

/* -----------------------------------------------------------------------
 * exp — Taylor series around 0 with range reduction
 *
 * exp(x) = exp(k * ln2 + r) = 2^k * exp(r)
 * where |r| <= ln2/2
 * ---------------------------------------------------------------------- */

double exp(double x) {
    if (isnan(x)) return NAN;
    if (x > 709.0) return HUGE_VAL;    /* overflow */
    if (x < -708.0) return 0.0;        /* underflow */

    /* Range reduction: x = k * ln(2) + r */
    int k = (int)(x / M_LN2 + (x >= 0.0 ? 0.5 : -0.5));
    double r = x - (double)k * M_LN2;

    /* Taylor series for exp(r), |r| < ln(2)/2 ~ 0.347 */
    double term = 1.0;
    double sum  = 1.0;
    for (int i = 1; i <= 25; i++) {
        term *= r / (double)i;
        sum += term;
        if (fabs(term) < 1e-16 * fabs(sum))
            break;
    }

    /* Multiply by 2^k */
    return ldexp(sum, k);
}

/* -----------------------------------------------------------------------
 * log — Newton's method or series
 *
 * We use the identity: ln(x) = ln(m * 2^e) = ln(m) + e * ln(2)
 * where 0.5 <= m < 1.0
 * Then ln(m) via the series ln((1+u)/(1-u)) = 2*(u + u^3/3 + u^5/5 + ...)
 * where u = (m-1)/(m+1)
 * ---------------------------------------------------------------------- */

double log(double x) {
    if (isnan(x) || x < 0.0) return NAN;
    if (x == 0.0) return -HUGE_VAL;
    if (isinf(x)) return HUGE_VAL;

    /* Decompose x = m * 2^e, 0.5 <= m < 1.0 */
    int e;
    double m = frexp(x, &e);

    /* u = (m - 1) / (m + 1); then ln(m) = 2 * sum(u^(2k+1) / (2k+1)) */
    /* Shift m to [sqrt(2)/2, sqrt(2)] for better convergence */
    if (m < 0.7071067811865476) {
        m *= 2.0;
        e--;
    }

    double u = (m - 1.0) / (m + 1.0);
    double u2 = u * u;
    double sum = 0.0;
    double term = u;
    for (int i = 0; i < 40; i++) {
        sum += term / (double)(2 * i + 1);
        term *= u2;
        if (fabs(term) < 1e-16)
            break;
    }
    sum *= 2.0;

    return sum + (double)e * M_LN2;
}

double log10(double x) {
    return log(x) / M_LN10;
}

double log2(double x) {
    return log(x) / M_LN2;
}

/* -----------------------------------------------------------------------
 * pow — x^y = exp(y * ln(x))
 *
 * Special cases handled for integer exponents and negative bases.
 * ---------------------------------------------------------------------- */

double pow(double base, double exponent) {
    /* Special cases */
    if (exponent == 0.0) return 1.0;
    if (base == 1.0) return 1.0;
    if (isnan(base) || isnan(exponent)) return NAN;
    if (base == 0.0) {
        if (exponent > 0.0) return 0.0;
        return HUGE_VAL;
    }

    /* Integer exponent — use repeated squaring */
    if (exponent == floor(exponent) && fabs(exponent) < 1e15) {
        long n = (long)exponent;
        int neg = 0;
        if (n < 0) { neg = 1; n = -n; }
        double result = 1.0;
        double b = base;
        while (n > 0) {
            if (n & 1) result *= b;
            b *= b;
            n >>= 1;
        }
        return neg ? 1.0 / result : result;
    }

    /* Negative base with non-integer exponent: NaN */
    if (base < 0.0)
        return NAN;

    return exp(exponent * log(base));
}

/* -----------------------------------------------------------------------
 * Trigonometric functions
 *
 * sin/cos via Taylor series with range reduction to [-pi, pi].
 * ---------------------------------------------------------------------- */

/* Reduce x to [-pi, pi] */
static double _reduce_angle(double x) {
    if (x >= -M_PI && x <= M_PI) return x;
    double twopi = 2.0 * M_PI;
    x = fmod(x, twopi);
    if (x > M_PI) x -= twopi;
    if (x < -M_PI) x += twopi;
    return x;
}

double sin(double x) {
    if (isnan(x) || isinf(x)) return NAN;
    x = _reduce_angle(x);

    /* Taylor series: sin(x) = x - x^3/3! + x^5/5! - ... */
    double term = x;
    double sum  = x;
    double x2   = x * x;
    for (int i = 1; i <= 20; i++) {
        term *= -x2 / (double)(2 * i * (2 * i + 1));
        sum += term;
        if (fabs(term) < 1e-16)
            break;
    }
    return sum;
}

double cos(double x) {
    if (isnan(x) || isinf(x)) return NAN;
    x = _reduce_angle(x);

    /* Taylor series: cos(x) = 1 - x^2/2! + x^4/4! - ... */
    double term = 1.0;
    double sum  = 1.0;
    double x2   = x * x;
    for (int i = 1; i <= 20; i++) {
        term *= -x2 / (double)((2 * i - 1) * (2 * i));
        sum += term;
        if (fabs(term) < 1e-16)
            break;
    }
    return sum;
}

double tan(double x) {
    double c = cos(x);
    if (c == 0.0) return copysign(HUGE_VAL, sin(x));
    return sin(x) / c;
}

/* -----------------------------------------------------------------------
 * atan — Maclaurin series with range reduction
 *
 * For |x| <= 1:  atan(x) = x - x^3/3 + x^5/5 - ...
 * For |x| > 1:   atan(x) = pi/2 - atan(1/x)
 * ---------------------------------------------------------------------- */

double atan(double x) {
    if (isnan(x)) return NAN;
    if (isinf(x)) return copysign(M_PI_2, x);

    int neg = 0;
    if (x < 0.0) { neg = 1; x = -x; }

    int invert = 0;
    if (x > 1.0) { invert = 1; x = 1.0 / x; }

    /* Further reduce using atan(x) = pi/6 + atan((x - 1/sqrt(3)) / (1 + x/sqrt(3))) */
    /* for x > 0.4142 (tan(pi/8)) */
    double offset = 0.0;
    if (x > 0.4142135623730950) {
        /* Use atan(x) = pi/4 + atan((x-1)/(x+1)) */
        offset = M_PI / 4.0;
        x = (x - 1.0) / (x + 1.0);
    }

    /* Taylor series for |x| <= tan(pi/8) ~ 0.414 */
    double x2 = x * x;
    double term = x;
    double sum  = x;
    for (int i = 1; i <= 30; i++) {
        term *= -x2;
        sum += term / (double)(2 * i + 1);
        if (fabs(term) < 1e-16)
            break;
    }

    double result = offset + sum;
    if (invert) result = M_PI_2 - result;
    return neg ? -result : result;
}

double atan2(double y, double x) {
    if (isnan(x) || isnan(y)) return NAN;

    if (x > 0.0)       return atan(y / x);
    if (x < 0.0 && y >= 0.0) return atan(y / x) + M_PI;
    if (x < 0.0 && y <  0.0) return atan(y / x) - M_PI;
    if (x == 0.0 && y >  0.0) return  M_PI_2;
    if (x == 0.0 && y <  0.0) return -M_PI_2;
    /* x == 0 && y == 0 */
    return 0.0;
}

/* -----------------------------------------------------------------------
 * Hyperbolic functions
 * ---------------------------------------------------------------------- */

double sinh(double x) {
    if (fabs(x) < 1e-10) return x;
    double e = exp(x);
    return (e - 1.0 / e) * 0.5;
}

double cosh(double x) {
    double e = exp(x);
    return (e + 1.0 / e) * 0.5;
}

double tanh(double x) {
    if (x > 20.0) return 1.0;
    if (x < -20.0) return -1.0;
    double e2 = exp(2.0 * x);
    return (e2 - 1.0) / (e2 + 1.0);
}
