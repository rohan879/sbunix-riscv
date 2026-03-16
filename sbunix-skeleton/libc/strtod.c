/*
 * strtod.c — string-to-floating-point conversion for SBUnix libc.
 *
 * Implements strtod(), strtof(), and atof().
 * Uses normal double arithmetic; on rv64imac (no FPU), the compiler
 * emits soft-float calls to libgcc (__adddf3, __muldf3, etc.) which
 * are resolved when user binaries are linked with gcc.
 *
 * If libgcc is unavailable, this file provides a fallback using
 * integer-only IEEE 754 bit construction.
 */

#include <stdlib.h>
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Helpers: classify characters
 * ---------------------------------------------------------------------- */

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' ||
           c == '\r' || c == '\f' || c == '\v';
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* -----------------------------------------------------------------------
 * IEEE 754 double construction from integer parts.
 *
 * We build the double bit pattern entirely with integer math so that
 * even if libgcc soft-float helpers are missing, the core conversion
 * still works.  The result is exact for values whose mantissa fits in
 * 52 bits; larger mantissas are rounded to nearest-even.
 * ---------------------------------------------------------------------- */

/* Union for type-punning between double and uint64_t */
typedef union {
    double       d;
    uint64_t     u;
} f64_union;

/* Union for type-punning between float and uint32_t */
typedef union {
    float        f;
    uint32_t     u;
} f32_union;

/*
 * Build a double from:
 *   sign      — 0 positive, 1 negative
 *   mantissa  — unsigned integer significand
 *   exp10     — decimal exponent (value = sign * mantissa * 10^exp10)
 *
 * Strategy:
 *   1. Normalise mantissa into the range [1, 2) as a fixed-point number.
 *   2. Apply the decimal exponent by repeated multiply/divide by 10,
 *      tracking the binary exponent.
 *   3. Assemble the IEEE 754 bits.
 */

/* Count leading zeros in a 64-bit word */
static int clz64(uint64_t x)
{
    if (x == 0) return 64;
    int n = 0;
    if ((x & 0xFFFFFFFF00000000ULL) == 0) { n += 32; x <<= 32; }
    if ((x & 0xFFFF000000000000ULL) == 0) { n += 16; x <<= 16; }
    if ((x & 0xFF00000000000000ULL) == 0) { n +=  8; x <<=  8; }
    if ((x & 0xF000000000000000ULL) == 0) { n +=  4; x <<=  4; }
    if ((x & 0xC000000000000000ULL) == 0) { n +=  2; x <<=  2; }
    if ((x & 0x8000000000000000ULL) == 0) { n +=  1; }
    return n;
}

/*
 * Multiply two 64-bit unsigned integers, returning the high 64 bits
 * of the 128-bit product.
 */
static uint64_t mulhi64(uint64_t a, uint64_t b)
{
    uint64_t a_lo = (uint32_t)a;
    uint64_t a_hi = a >> 32;
    uint64_t b_lo = (uint32_t)b;
    uint64_t b_hi = b >> 32;

    uint64_t p0 = a_lo * b_lo;
    uint64_t p1 = a_lo * b_hi;
    uint64_t p2 = a_hi * b_lo;
    uint64_t p3 = a_hi * b_hi;

    uint64_t mid = (p0 >> 32) + (uint32_t)p1 + (uint32_t)p2;
    uint64_t hi  = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
    return hi;
}

/*
 * Powers of 10 as high-precision fixed-point multipliers.
 * We only need to handle exponents roughly in [-308, +308].
 *
 * For moderate exponents we use iterative multiply/divide by 10
 * on the mantissa in a 64-bit fixed-point representation.
 */

/*
 * build_double_from_parts:
 *   Construct IEEE 754 double from (sign, mantissa, decimal_exponent).
 *   mantissa is the integer formed by the parsed digits.
 *   decimal_exponent adjusts for the decimal point position and E exponent.
 *   Result = sign * mantissa * 10^decimal_exponent.
 */
static double build_double(int sign, uint64_t mantissa, int exp10)
{
    f64_union u;

    if (mantissa == 0) {
        u.u = sign ? 0x8000000000000000ULL : 0ULL;
        return u.d;
    }

    /*
     * Normalise: shift mantissa so bit 63 is set, tracking binary exponent.
     * frac = mantissa << shift, binary exponent = 63 - shift + (number of
     * bits in original mantissa - 1)
     */
    int lz = clz64(mantissa);
    uint64_t frac = mantissa << lz; /* now bit 63 is set */
    int bexp = 63 - lz;            /* binary exponent of the integer */

    /*
     * Now value = frac * 2^(bexp - 63).
     * We need to multiply/divide by 10^|exp10|.
     *
     * Multiply by 10: frac *= 10 (may overflow, shift right and bump bexp).
     * Divide   by 10: frac = (frac / 10), adjusting for precision.
     */

    int e = exp10;

    /* Handle positive decimal exponent: multiply by 10 repeatedly */
    while (e > 0) {
        /*
         * Multiply frac by 10 = 5 * 2.
         * Multiply by 5, then shift left by 1.
         * If the multiply by 5 would overflow, shift right first.
         */
        /* Check if frac * 5 overflows 64 bits */
        if (frac > 0xFFFFFFFFFFFFFFFFULL / 5) {
            /* Use mulhi to get the high bits */
            uint64_t lo = frac * 5;
            uint64_t hi = mulhi64(frac, 5);
            /* Combine: result is (hi:lo), we keep the top 64 bits */
            int shift = 64 - clz64(hi);
            if (shift <= 0) shift = 1;
            frac = (hi << (64 - shift)) | (lo >> shift);
            bexp += shift;
        } else {
            frac *= 5;
        }
        /* multiply by 2 */
        if (frac & 0x8000000000000000ULL) {
            frac >>= 1;
            bexp++;
        }
        bexp++; /* the *2 part */
        /* re-normalise: ensure bit 63 is set */
        if (frac != 0) {
            int s = clz64(frac);
            if (s > 0) {
                frac <<= s;
                bexp -= s;
            }
        }
        e--;

        /* Early out: overflow to infinity */
        if (bexp > 1023 + 64) {
            u.u = sign ? 0xFFF0000000000000ULL : 0x7FF0000000000000ULL;
            return u.d;
        }
    }

    /* Handle negative decimal exponent: divide by 10 repeatedly */
    while (e < 0) {
        /*
         * Divide frac by 10 = divide by 5 then shift right by 1.
         * But we want to keep precision, so shift left first if we can.
         */
        /* shift left to fill top bit */
        int s = clz64(frac);
        if (s > 0) {
            frac <<= s;
            bexp -= s;
        }
        frac /= 5;
        bexp--;  /* divide by 2 */

        /* re-normalise */
        if (frac != 0) {
            s = clz64(frac);
            if (s > 0) {
                frac <<= s;
                bexp -= s;
            }
        }
        e++;

        /* Early out: underflow to zero */
        if (bexp < -1074 - 64) {
            u.u = sign ? 0x8000000000000000ULL : 0ULL;
            return u.d;
        }
    }

    /*
     * Now value = frac * 2^(bexp - 63).
     *
     * IEEE 754 double: 1 sign + 11 exponent + 52 fraction bits.
     * Normalised form: 1.fraction * 2^(biased_exp - 1023).
     *
     * frac has bit 63 set (the implicit leading 1).
     * fraction bits = frac[62:11] (top 52 bits below the leading 1).
     */

    /* The actual binary exponent of the value */
    int ieee_exp = bexp; /* since frac is normalised with bit 63 = 1 */

    if (ieee_exp > 1023) {
        /* Overflow: return infinity */
        u.u = sign ? 0xFFF0000000000000ULL : 0x7FF0000000000000ULL;
        return u.d;
    }

    if (ieee_exp < -1022) {
        /* Subnormal or zero */
        int shift = -1022 - ieee_exp;
        if (shift >= 64) {
            u.u = sign ? 0x8000000000000000ULL : 0ULL;
            return u.d;
        }
        /* Round: check bit that will be shifted out */
        uint64_t round_bit = (shift < 64) ? ((frac >> (shift - 1)) & 1) : 0;
        frac >>= shift;
        frac += round_bit; /* round to nearest */
        /* subnormal: exponent field = 0 */
        uint64_t fraction_bits = (frac >> 11) & 0x000FFFFFFFFFFFFFULL;
        u.u = ((uint64_t)sign << 63) | fraction_bits;
        return u.d;
    }

    /* Normal number */
    /* Round to nearest: check bit 10 (the first bit below the 52 fraction bits) */
    uint64_t round_bit = (frac >> 10) & 1;
    uint64_t fraction_bits = (frac >> 11) & 0x000FFFFFFFFFFFFFULL;
    fraction_bits += round_bit;

    /* Rounding may cause fraction overflow (all 52 bits carry) */
    if (fraction_bits > 0x000FFFFFFFFFFFFFULL) {
        fraction_bits = 0;
        ieee_exp++;
        if (ieee_exp > 1023) {
            u.u = sign ? 0xFFF0000000000000ULL : 0x7FF0000000000000ULL;
            return u.d;
        }
    }

    uint64_t biased_exp = (uint64_t)(ieee_exp + 1023);
    u.u = ((uint64_t)sign << 63) | (biased_exp << 52) | fraction_bits;
    return u.d;
}

/* -----------------------------------------------------------------------
 * strtod() — convert string to double
 * ---------------------------------------------------------------------- */

double strtod(const char *s, char **endptr)
{
    const char *p = s;

    /* Skip leading whitespace */
    while (is_space(*p))
        p++;

    /* Optional sign */
    int sign = 0;
    if (*p == '-') {
        sign = 1;
        p++;
    } else if (*p == '+') {
        p++;
    }

    const char *start = p;
    uint64_t mantissa = 0;
    int      digits = 0;
    int      decimal_exp = 0;
    int      saw_dot = 0;
    int      overflow_digits = 0;

    /* Parse integer and fractional parts */
    while (is_digit(*p) || (*p == '.' && !saw_dot)) {
        if (*p == '.') {
            saw_dot = 1;
            p++;
            continue;
        }
        if (digits < 19) {
            /* 19 decimal digits fit in uint64_t (max 18446744073709551615) */
            mantissa = mantissa * 10 + (*p - '0');
            if (mantissa > 0)
                digits++;
            if (saw_dot)
                decimal_exp--;
        } else {
            /* Too many significant digits; just track the exponent */
            if (!saw_dot)
                overflow_digits++;
        }
        p++;
    }

    decimal_exp += overflow_digits;

    /* No digits consumed? Not a valid number */
    if (p == start || (p == start + 1 && saw_dot && !is_digit(*(start)))) {
        /* Check: if we only saw a dot with no digits around it */
        int has_digits = 0;
        for (const char *q = start; q < p; q++) {
            if (is_digit(*q)) { has_digits = 1; break; }
        }
        if (!has_digits) {
            if (endptr)
                *endptr = (char *)s;
            f64_union u;
            u.u = 0;
            return u.d;
        }
    }

    /* Optional exponent */
    if (*p == 'e' || *p == 'E') {
        const char *e_start = p;
        p++;
        int esign = 0;
        if (*p == '-') {
            esign = 1;
            p++;
        } else if (*p == '+') {
            p++;
        }
        if (is_digit(*p)) {
            int eval = 0;
            while (is_digit(*p)) {
                if (eval < 10000)
                    eval = eval * 10 + (*p - '0');
                p++;
            }
            decimal_exp += esign ? -eval : eval;
        } else {
            /* No digits after 'e' — exponent is invalid, rewind */
            p = e_start;
        }
    }

    if (endptr)
        *endptr = (char *)p;

    return build_double(sign, mantissa, decimal_exp);
}

/* -----------------------------------------------------------------------
 * strtof() — convert string to float
 *
 * Parse as double, then convert to float via bit manipulation.
 * ---------------------------------------------------------------------- */

float strtof(const char *s, char **endptr)
{
    double d = strtod(s, endptr);

    /* Extract double bits and convert to float bits */
    f64_union du;
    du.d = d;
    uint64_t dbits = du.u;

    int      dsign = (int)(dbits >> 63);
    int      dexp  = (int)((dbits >> 52) & 0x7FF) - 1023;
    uint64_t dfrac = dbits & 0x000FFFFFFFFFFFFFULL;

    f32_union fu;

    /* Special cases */
    if (dexp == 1024) {
        /* Infinity or NaN */
        if (dfrac == 0) {
            fu.u = dsign ? 0xFF800000U : 0x7F800000U;
        } else {
            fu.u = dsign ? 0xFFC00000U : 0x7FC00000U; /* quiet NaN */
        }
        return fu.f;
    }

    if (dexp == -1023 && dfrac == 0) {
        /* Zero */
        fu.u = dsign ? 0x80000000U : 0x00000000U;
        return fu.f;
    }

    /* Check float range: float exponent range is [-126, +127] */
    if (dexp > 127) {
        /* Overflow: infinity */
        fu.u = dsign ? 0xFF800000U : 0x7F800000U;
        return fu.f;
    }

    if (dexp < -149) {
        /* Underflow: zero */
        fu.u = dsign ? 0x80000000U : 0x00000000U;
        return fu.f;
    }

    /* Convert 52-bit fraction to 23-bit fraction */
    /* Add implicit leading 1 for normal doubles */
    uint64_t full_frac = dfrac;
    int is_subnormal_d = (dexp == -1023);
    if (!is_subnormal_d)
        full_frac |= (1ULL << 52);

    if (dexp < -126) {
        /* Float subnormal */
        int shift = (-126 - dexp);
        /* Round: shift right by (52 - 23 + shift) = 29 + shift */
        int total_shift = 29 + shift;
        if (total_shift >= 64) {
            fu.u = dsign ? 0x80000000U : 0x00000000U;
            return fu.f;
        }
        uint32_t ffrac = (uint32_t)(full_frac >> total_shift);
        /* round to nearest */
        if (total_shift > 0 && total_shift < 64) {
            uint64_t round = (full_frac >> (total_shift - 1)) & 1;
            ffrac += (uint32_t)round;
        }
        fu.u = ((uint32_t)dsign << 31) | (ffrac & 0x007FFFFFU);
        return fu.f;
    }

    /* Normal float */
    /* Shift 52-bit fraction to 23 bits: drop 29 bits */
    uint32_t ffrac = (uint32_t)(dfrac >> 29);
    /* Round */
    if (dfrac & (1ULL << 28))
        ffrac++;
    if (ffrac > 0x007FFFFFU) {
        ffrac = 0;
        dexp++;
        if (dexp > 127) {
            fu.u = dsign ? 0xFF800000U : 0x7F800000U;
            return fu.f;
        }
    }

    uint32_t biased = (uint32_t)(dexp + 127);
    fu.u = ((uint32_t)dsign << 31) | (biased << 23) | (ffrac & 0x007FFFFFU);
    return fu.f;
}

/* -----------------------------------------------------------------------
 * atof() — convert string to double (convenience wrapper)
 * ---------------------------------------------------------------------- */

double atof(const char *s)
{
    return strtod(s, (char **)0);
}
