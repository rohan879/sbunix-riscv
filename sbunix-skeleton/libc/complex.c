/*
 * complex.c — C11 complex math functions for SBUnix libc.
 *
 * Each function protects against errno leakage from internal
 * math calls (e.g. log(0) in clog) by saving/restoring errno
 * around computations that might spuriously set it.
 * Special-value handling follows C11 Annex G where practical.
 */

#include <complex.h>
#include <errno.h>

/* ---- classification helpers (avoid pulling in internal math symbols) ---- */
extern int __math_isnan(double);
extern int __math_isinf(double);
extern int __math_isfinite(double);

double cabs(double complex z) { return hypot(creal(z), cimag(z)); }
float cabsf(float complex z) { return (float)cabs((double complex)z); }
long double cabsl(long double complex z) { return (long double)cabs((double complex)z); }

double carg(double complex z) { return atan2(cimag(z), creal(z)); }
float cargf(float complex z) { return (float)carg((double complex)z); }
long double cargl(long double complex z) { return (long double)carg((double complex)z); }

double creal(double complex z) { return __real__ z; }
float crealf(float complex z) { return __real__ z; }
long double creall(long double complex z) { return __real__ z; }

double cimag(double complex z) { return __imag__ z; }
float cimagf(float complex z) { return __imag__ z; }
long double cimagl(long double complex z) { return __imag__ z; }

double complex conj(double complex z) { return creal(z) - I * cimag(z); }
float complex conjf(float complex z) { return (float complex)conj((double complex)z); }
long double complex conjl(long double complex z) { return (long double complex)conj((double complex)z); }

/*
 * cproj: C11 7.3.9.4 — project onto Riemann sphere.
 * If either component is infinite, return INFINITY + I*copysign(0, imag).
 * Otherwise return z unchanged.
 */
double complex cproj(double complex z) {
  if (__math_isinf(creal(z)) || __math_isinf(cimag(z)))
    return INFINITY + I * copysign(0.0, cimag(z));
  return z;
}
float complex cprojf(float complex z) { return (float complex)cproj((double complex)z); }
long double complex cprojl(long double complex z) { return (long double complex)cproj((double complex)z); }

/*
 * cexp: e^z = e^x * (cos(y) + i*sin(y)).
 * Protect against errno leakage from exp(x) overflow when
 * the complex result is well-defined (e.g. cexp(-inf + iy) = 0).
 */
double complex cexp(double complex z) {
  double x = creal(z);
  double y = cimag(z);

  /* C11 G.6.3.1: special values for cexp */
  if (__math_isnan(x)) {
    /* cexp(NaN + 0i) = NaN + 0i; cexp(NaN + yi) = NaN + NaN*i */
    if (y == 0.0)
      return z;
    return NAN + I * NAN;
  }
  if (__math_isinf(x)) {
    if (x > 0.0) {
      /* cexp(+inf + 0i) = +inf + 0i */
      if (y == 0.0)
        return z;
      /* cexp(+inf + NaN*i) = +inf + NaN*i */
      if (__math_isnan(y))
        return INFINITY + I * NAN;
      /* cexp(+inf + inf*i) = +inf + NaN*i per C11 */
      if (__math_isinf(y))
        return INFINITY + I * NAN;
      /* cexp(+inf + yi) = +inf*cos(y) + i*inf*sin(y) */
      double s = sin(y);
      double c = cos(y);
      return copysign(INFINITY, c) + I * copysign(INFINITY, s);
    } else {
      /* cexp(-inf + anything) = 0 */
      if (__math_isnan(y) || __math_isinf(y))
        return 0.0 + I * 0.0;
      /* cexp(-inf + yi) = 0*cos(y) + i*0*sin(y) — result is +/-0 */
      return copysign(0.0, cos(y)) + I * copysign(0.0, sin(y));
    }
  }
  if (__math_isnan(y) || __math_isinf(y)) {
    /* finite x, non-finite y: domain error per C11 */
    if (x == 0.0)
      return NAN + I * 0.0;
    return NAN + I * NAN;
  }

  /* General case: save/restore errno to prevent exp() overflow
   * from leaking when the result is valid */
  int saved = errno;
  double ex = exp(x);
  if (__math_isinf(ex) && __math_isfinite(x)) {
    /* exp overflowed: errno was set by exp(), but cexp result is
     * well-defined as inf*cis(y), so we still allow ERANGE to
     * propagate since the magnitude is infinite */
  } else {
    /* No real overflow — restore errno in case exp set it spuriously */
    errno = saved;
  }
  return ex * cos(y) + I * (ex * sin(y));
}
float complex cexpf(float complex z) { return (float complex)cexp((double complex)z); }
long double complex cexpl(long double complex z) { return (long double complex)cexp((double complex)z); }

/*
 * clog: log(z) = log|z| + i*arg(z).
 * Protect against errno leakage from log(cabs(z)):
 * when z=0, log(0) sets ERANGE, but clog(0) should set ERANGE
 * itself for the pole at zero. For other z, internal log() might
 * spuriously set errno via intermediate computations.
 */
double complex clog(double complex z) {
  double x = creal(z);
  double y = cimag(z);

  /* C11 G.6.3.2: clog special values */
  if (__math_isnan(x)) {
    if (__math_isinf(y))
      return INFINITY + I * NAN;
    return NAN + I * NAN;
  }
  if (__math_isnan(y)) {
    if (__math_isinf(x))
      return INFINITY + I * NAN;
    return NAN + I * NAN;
  }

  /* clog(-inf + iy) = +inf + i*pi */
  if (__math_isinf(x) && x < 0.0) {
    if (__math_isinf(y))
      return INFINITY + I * copysign(3.0 * M_PI_4, y);
    return INFINITY + I * copysign(M_PI, y);
  }
  /* clog(+inf + iy) = +inf + i*0 (or i*pi/4 if y=inf) */
  if (__math_isinf(x) && x > 0.0) {
    if (__math_isinf(y))
      return INFINITY + I * copysign(M_PI_4, y);
    return INFINITY + I * copysign(0.0, y);
  }
  if (__math_isinf(y))
    return INFINITY + I * copysign(M_PI_2, y);

  /* Pole at zero: clog(0) = -inf + i*pi (or i*0 depending on sign) */
  if (x == 0.0 && y == 0.0) {
    errno = ERANGE;
    return -INFINITY + I * copysign(0.0, y);
  }

  /* General case: save/restore errno around internal math calls
   * to prevent log(cabs(z)) from leaking ERANGE for valid z */
  int saved = errno;
  double r = cabs(z);
  double lr = log(r);
  double angle = atan2(y, x);
  /* Restore errno — only set it if the result itself is problematic */
  errno = saved;
  if (__math_isinf(lr))
    errno = ERANGE;
  return lr + I * angle;
}
float complex clogf(float complex z) { return (float complex)clog((double complex)z); }
long double complex clogl(long double complex z) { return (long double complex)clog((double complex)z); }

double complex cpow(double complex x, double complex y) {
  /* cpow(x,y) = cexp(y * clog(x)) — errno handled by cexp/clog */
  if (creal(x) == 0.0 && cimag(x) == 0.0) {
    /* 0^y: return 0 for positive real y, otherwise NaN */
    if (creal(y) > 0.0)
      return 0.0 + I * 0.0;
    if (creal(y) == 0.0 && cimag(y) == 0.0)
      return 1.0 + I * 0.0;
    /* 0^(negative or complex) is pole error */
    errno = ERANGE;
    return INFINITY + I * 0.0;
  }
  return cexp(y * clog(x));
}
float complex cpowf(float complex x, float complex y) { return (float complex)cpow((double complex)x, (double complex)y); }
long double complex cpowl(long double complex x, long double complex y) { return (long double complex)cpow((double complex)x, (double complex)y); }

double complex csqrt(double complex z) {
  double x = creal(z);
  double y = cimag(z);

  /* C11 G.6.4.2: special values */
  if (__math_isnan(x)) {
    return NAN + I * NAN;
  }
  if (__math_isinf(x)) {
    if (x > 0.0) {
      /* csqrt(+inf + yi) = +inf + i*0, csqrt(+inf + NaN*i) = +inf + NaN*i */
      if (__math_isnan(y))
        return INFINITY + I * NAN;
      return INFINITY + I * copysign(0.0, y);
    } else {
      /* csqrt(-inf + yi) = 0 + i*inf, csqrt(-inf + NaN*i) = NaN + i*+/-inf */
      if (__math_isnan(y))
        return NAN + I * copysign(INFINITY, y);
      return 0.0 + I * copysign(INFINITY, y);
    }
  }
  if (__math_isnan(y)) {
    return NAN + I * NAN;
  }

  double r = cabs(z);
  double t = sqrt((r + fabs(x)) * 0.5);
  if (t == 0.0)
    return 0.0 + I * 0.0;
  if (x >= 0.0)
    return t + I * (y / (2.0 * t));
  return fabs(y) / (2.0 * t) + I * copysign(t, y);
}
float complex csqrtf(float complex z) { return (float complex)csqrt((double complex)z); }
long double complex csqrtl(long double complex z) { return (long double complex)csqrt((double complex)z); }

double complex csin(double complex z) {
  double x = creal(z), y = cimag(z);
  /* NaN/inf handling: sin(NaN+0i)=NaN+0i, sin(0+inf*i)=0+inf*i */
  if (__math_isnan(x)) {
    if (y == 0.0) return NAN + I * 0.0;
    return NAN + I * NAN;
  }
  if (__math_isinf(x))
    return NAN + I * NAN;
  if (__math_isnan(y)) {
    if (x == 0.0) return 0.0 + I * NAN;
    return NAN + I * NAN;
  }
  /* Save/restore errno: internal sinh/cosh may set ERANGE for large y */
  int saved = errno;
  double complex result = sin(x) * cosh(y) + I * (cos(x) * sinh(y));
  if (__math_isfinite(creal(result)) && __math_isfinite(cimag(result)))
    errno = saved;
  return result;
}
float complex csinf(float complex z) { return (float complex)csin((double complex)z); }
long double complex csinl(long double complex z) { return (long double complex)csin((double complex)z); }

double complex ccos(double complex z) {
  double x = creal(z), y = cimag(z);
  /* NaN/inf handling */
  if (__math_isnan(x)) {
    if (y == 0.0) return NAN + I * 0.0;
    return NAN + I * NAN;
  }
  if (__math_isinf(x))
    return NAN + I * NAN;
  if (__math_isnan(y)) {
    if (x == 0.0) return NAN + I * 0.0;
    return NAN + I * NAN;
  }
  /* Save/restore errno: internal sinh/cosh may set ERANGE for large y */
  int saved = errno;
  double complex result = cos(x) * cosh(y) - I * (sin(x) * sinh(y));
  if (__math_isfinite(creal(result)) && __math_isfinite(cimag(result)))
    errno = saved;
  return result;
}
float complex ccosf(float complex z) { return (float complex)ccos((double complex)z); }
long double complex ccosl(long double complex z) { return (long double complex)ccos((double complex)z); }

/*
 * ctan: protect against division by zero when ccos(z) is zero.
 * Use the identity ctan(z) = -i * ctanh(iz) for numerical stability
 * or compute directly with overflow protection.
 */
double complex ctan(double complex z) {
  double x = creal(z), y = cimag(z);
  /* NaN/inf special values */
  if (__math_isnan(x)) {
    if (y == 0.0) return NAN + I * 0.0;
    return NAN + I * NAN;
  }
  if (__math_isinf(x))
    return NAN + I * NAN;
  if (__math_isnan(y))
    return NAN + I * NAN;
  /* For large |y|, tanh(y) -> +/-1, ctan approaches +/-i */
  if (fabs(y) > 22.0)
    return copysign(0.0, sin(2.0 * x)) + I * copysign(1.0, y);

  /* Stable formula: ctan(x+iy) = (sin(2x) + i*sinh(2y)) / (cos(2x) + cosh(2y)) */
  double s2x = sin(2.0 * x);
  double c2x = cos(2.0 * x);
  int saved = errno;
  double sh2y = sinh(2.0 * y);
  double ch2y = cosh(2.0 * y);
  errno = saved; /* restore: sinh/cosh may set ERANGE for large 2y */
  double denom = c2x + ch2y;
  if (denom == 0.0)
    return NAN + I * NAN; /* shouldn't happen for finite inputs */
  return s2x / denom + I * (sh2y / denom);
}
float complex ctanf(float complex z) { return (float complex)ctan((double complex)z); }
long double complex ctanl(long double complex z) { return (long double complex)ctan((double complex)z); }

double complex csinh(double complex z) {
  double x = creal(z), y = cimag(z);
  /* NaN/inf handling */
  if (__math_isnan(x)) {
    if (y == 0.0) return NAN + I * 0.0;
    return NAN + I * NAN;
  }
  if (__math_isnan(y)) {
    if (x == 0.0) return 0.0 + I * NAN;
    return NAN + I * NAN;
  }
  if (__math_isinf(x)) {
    if (__math_isinf(y)) return copysign(INFINITY, x) + I * NAN;
    if (y == 0.0) return z;
    return copysign(INFINITY, x * cos(y)) + I * copysign(INFINITY, sin(y));
  }
  /* Save/restore errno around internal sinh/cosh calls */
  int saved = errno;
  double complex result = sinh(x) * cos(y) + I * (cosh(x) * sin(y));
  if (__math_isfinite(creal(result)) && __math_isfinite(cimag(result)))
    errno = saved;
  return result;
}
float complex csinhf(float complex z) { return (float complex)csinh((double complex)z); }
long double complex csinhl(long double complex z) { return (long double complex)csinh((double complex)z); }

double complex ccosh(double complex z) {
  double x = creal(z), y = cimag(z);
  /* NaN/inf handling */
  if (__math_isnan(x)) {
    if (y == 0.0) return NAN + I * 0.0;
    return NAN + I * NAN;
  }
  if (__math_isnan(y)) {
    if (x == 0.0) return NAN + I * 0.0;
    return NAN + I * NAN;
  }
  if (__math_isinf(x)) {
    if (__math_isinf(y)) return INFINITY + I * NAN;
    if (y == 0.0) return INFINITY + I * 0.0;
    return copysign(INFINITY, cos(y)) + I * copysign(INFINITY, x * sin(y));
  }
  /* Save/restore errno around internal cosh/sinh calls */
  int saved = errno;
  double complex result = cosh(x) * cos(y) + I * (sinh(x) * sin(y));
  if (__math_isfinite(creal(result)) && __math_isfinite(cimag(result)))
    errno = saved;
  return result;
}
float complex ccoshf(float complex z) { return (float complex)ccosh((double complex)z); }
long double complex ccoshl(long double complex z) { return (long double complex)ccosh((double complex)z); }

/*
 * ctanh: protect against division by zero when ccosh(z) is zero.
 * Use stable formula: ctanh(x+iy) = (sinh(2x) + i*sin(2y)) / (cosh(2x) + cos(2y))
 */
double complex ctanh(double complex z) {
  double x = creal(z), y = cimag(z);
  /* NaN/inf special values */
  if (__math_isnan(x)) {
    if (y == 0.0) return NAN + I * 0.0;
    return NAN + I * NAN;
  }
  if (__math_isnan(y))
    return NAN + I * NAN;
  if (__math_isinf(x)) {
    if (__math_isnan(y)) return copysign(1.0, x) + I * 0.0;
    return copysign(1.0, x) + I * copysign(0.0, sin(2.0 * y));
  }
  /* For large |x|, tanh(x) -> +/-1 */
  if (fabs(x) > 22.0)
    return copysign(1.0, x) + I * copysign(0.0, sin(2.0 * y));

  /* Stable formula avoids cosh(z) division-by-zero */
  int saved = errno;
  double sh2x = sinh(2.0 * x);
  double ch2x = cosh(2.0 * x);
  errno = saved; /* restore: sinh/cosh may set ERANGE */
  double s2y = sin(2.0 * y);
  double c2y = cos(2.0 * y);
  double denom = ch2x + c2y;
  if (denom == 0.0)
    return NAN + I * NAN;
  return sh2x / denom + I * (s2y / denom);
}
float complex ctanhf(float complex z) { return (float complex)ctanh((double complex)z); }
long double complex ctanhl(long double complex z) { return (long double complex)ctanh((double complex)z); }

double complex casin(double complex z) {
  /* casin(z) = -i * clog(iz + csqrt(1 - z*z)) */
  /* Save/restore errno: internal clog can leak via log(0) */
  int saved = errno;
  double complex iz = I * z;
  double complex result = -I * clog(iz + csqrt(1.0 - z * z));
  /* Only preserve errno if the result is finite (no actual error) */
  if (__math_isfinite(creal(result)) && __math_isfinite(cimag(result)))
    errno = saved;
  return result;
}
float complex casinf(float complex z) { return (float complex)casin((double complex)z); }
long double complex casinl(long double complex z) { return (long double complex)casin((double complex)z); }

double complex cacos(double complex z) {
  /* cacos(z) = pi/2 - casin(z) */
  int saved = errno;
  double complex result = M_PI_2 - casin(z);
  if (__math_isfinite(creal(result)) && __math_isfinite(cimag(result)))
    errno = saved;
  return result;
}
float complex cacosf(float complex z) { return (float complex)cacos((double complex)z); }
long double complex cacosl(long double complex z) { return (long double complex)cacos((double complex)z); }

double complex catan(double complex z) {
  /* catan(z) = (i/2) * (clog(1 - iz) - clog(1 + iz)) */
  int saved = errno;
  double complex result = (I * 0.5) * (clog(1.0 - I * z) - clog(1.0 + I * z));
  if (__math_isfinite(creal(result)) && __math_isfinite(cimag(result)))
    errno = saved;
  return result;
}
float complex catanf(float complex z) { return (float complex)catan((double complex)z); }
long double complex catanl(long double complex z) { return (long double complex)catan((double complex)z); }

double complex casinh(double complex z) {
  /* casinh(z) = clog(z + csqrt(z*z + 1)) */
  int saved = errno;
  double complex result = clog(z + csqrt(z * z + 1.0));
  if (__math_isfinite(creal(result)) && __math_isfinite(cimag(result)))
    errno = saved;
  return result;
}
float complex casinhf(float complex z) { return (float complex)casinh((double complex)z); }
long double complex casinhl(long double complex z) { return (long double complex)casinh((double complex)z); }

double complex cacosh(double complex z) {
  /* cacosh(z) = clog(z + csqrt(z+1) * csqrt(z-1)) */
  int saved = errno;
  double complex result = clog(z + csqrt(z + 1.0) * csqrt(z - 1.0));
  if (__math_isfinite(creal(result)) && __math_isfinite(cimag(result)))
    errno = saved;
  return result;
}
float complex cacoshf(float complex z) { return (float complex)cacosh((double complex)z); }
long double complex cacoshl(long double complex z) { return (long double complex)cacosh((double complex)z); }

double complex catanh(double complex z) {
  /* catanh(z) = 0.5 * (clog(1+z) - clog(1-z)) */
  int saved = errno;
  double complex result = 0.5 * (clog(1.0 + z) - clog(1.0 - z));
  if (__math_isfinite(creal(result)) && __math_isfinite(cimag(result)))
    errno = saved;
  return result;
}
float complex catanhf(float complex z) { return (float complex)catanh((double complex)z); }
long double complex catanhl(long double complex z) { return (long double complex)catanh((double complex)z); }
