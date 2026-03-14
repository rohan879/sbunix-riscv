/*
 * mathtest — integer math and arithmetic test
 *
 * Tests integer arithmetic operations, overflow handling,
 * abs/min/max, and division/modulo edge cases.
 * (Floating-point math cannot be tested because the rv64imac
 *  toolchain lacks soft-float libgcc support for double/float.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] %s\n", label); g_pass++; } \
    else { printf("[FAIL] %s\n", label); g_fail++; } \
} while (0)

/* -----------------------------------------------------------------------
 * 1. abs — integer absolute value
 * ---------------------------------------------------------------------- */

static void test_abs(void)
{
    CHECK("abs(-5) == 5", abs(-5) == 5);
    CHECK("abs(5) == 5",  abs(5) == 5);
    CHECK("abs(0) == 0",  abs(0) == 0);
    CHECK("abs(-1) == 1", abs(-1) == 1);
}

/* -----------------------------------------------------------------------
 * 2. integer division and modulo
 * ---------------------------------------------------------------------- */

static void test_divmod(void)
{
    CHECK("7 / 3 == 2",   7 / 3 == 2);
    CHECK("7 % 3 == 1",   7 % 3 == 1);
    CHECK("10 / 3 == 3",  10 / 3 == 3);
    CHECK("10 % 3 == 1",  10 % 3 == 1);
    CHECK("6 / 3 == 2",   6 / 3 == 2);
    CHECK("6 % 3 == 0",   6 % 3 == 0);
    CHECK("-7 / 3 == -2", -7 / 3 == -2);
    CHECK("-7 % 3 == -1", -7 % 3 == -1);
}

/* -----------------------------------------------------------------------
 * 3. atoi / strtol — string to integer conversion
 * ---------------------------------------------------------------------- */

static void test_strtol(void)
{
    CHECK("atoi('42') == 42",      atoi("42") == 42);
    CHECK("atoi('-99') == -99",    atoi("-99") == -99);
    CHECK("atoi('0') == 0",        atoi("0") == 0);

    char *end;
    long val = strtol("  -123abc", &end, 10);
    CHECK("strtol('-123abc') == -123", val == -123);
    CHECK("strtol stops at 'a'",       *end == 'a');

    val = strtol("0xFF", &end, 16);
    CHECK("strtol('0xFF', 16) == 255", val == 255);

    val = strtol("0777", &end, 8);
    CHECK("strtol('0777', 8) == 511", val == 511);
}

/* -----------------------------------------------------------------------
 * 4. 64-bit arithmetic
 * ---------------------------------------------------------------------- */

static void test_64bit(void)
{
    long a = 1L << 40;
    long b = 1L << 40;
    long sum = a + b;
    CHECK("(1<<40) + (1<<40) == (1<<41)", sum == (1L << 41));

    long c = 1000000000L * 1000000000L;
    CHECK("1e9 * 1e9 == 1e18", c == 1000000000000000000L);

    unsigned long u = (unsigned long)-1;
    CHECK("(unsigned long)-1 == ULONG_MAX",
          u == (unsigned long)ULONG_MAX);
}

/* -----------------------------------------------------------------------
 * 5. bit manipulation
 * ---------------------------------------------------------------------- */

static void test_bits(void)
{
    unsigned long x = 0xDEADBEEF;
    CHECK("x & 0xFF == 0xEF", (x & 0xFF) == 0xEF);
    CHECK("x >> 16 & 0xFFFF == 0xDEAD",
          ((x >> 16) & 0xFFFF) == 0xDEAD);
    CHECK("x | 0x100 sets bit 8",
          (x | 0x100) == 0xDEADBFEF);
    CHECK("x ^ x == 0", (x ^ x) == 0);
    CHECK("~0UL == ULONG_MAX", ~0UL == (unsigned long)ULONG_MAX);
}

/* -----------------------------------------------------------------------
 * 6. integer min/max (manual)
 * ---------------------------------------------------------------------- */

static void test_minmax(void)
{
    int a = 10, b = 20;
    int mn = (a < b) ? a : b;
    int mx = (a > b) ? a : b;
    CHECK("min(10,20) == 10", mn == 10);
    CHECK("max(10,20) == 20", mx == 20);

    mn = (b < a) ? b : a;
    mx = (b > a) ? b : a;
    CHECK("min(20,10) == 10", mn == 10);
    CHECK("max(20,10) == 20", mx == 20);
}

/* -----------------------------------------------------------------------
 * 7. integer floor-division (rounding toward negative infinity)
 * ---------------------------------------------------------------------- */

static long floor_div(long a, long b)
{
    long q = a / b;
    /* Adjust if remainder has opposite sign from divisor */
    if ((a % b != 0) && ((a ^ b) < 0))
        q--;
    return q;
}

static void test_floor_div(void)
{
    CHECK("floor_div(7, 3) == 2",    floor_div(7, 3) == 2);
    CHECK("floor_div(-7, 3) == -3",  floor_div(-7, 3) == -3);
    CHECK("floor_div(7, -3) == -3",  floor_div(7, -3) == -3);
    CHECK("floor_div(-7, -3) == 2",  floor_div(-7, -3) == 2);
    CHECK("floor_div(6, 3) == 2",    floor_div(6, 3) == 2);
}

/* --- Main --- */

int main(void)
{
    printf("=== mathtest: integer math ===\n");

    test_abs();
    test_divmod();
    test_strtol();
    test_64bit();
    test_bits();
    test_minmax();
    test_floor_div();

    printf("=== mathtest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
