#include <complex.h>

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

double complex cproj(double complex z) { return z; }
float complex cprojf(float complex z) { return z; }
long double complex cprojl(long double complex z) { return z; }

double complex cexp(double complex z) {
  double x = creal(z);
  double y = cimag(z);
  double ex = exp(x);
  return ex * cos(y) + I * (ex * sin(y));
}
float complex cexpf(float complex z) { return (float complex)cexp((double complex)z); }
long double complex cexpl(long double complex z) { return (long double complex)cexp((double complex)z); }

double complex clog(double complex z) {
  return log(cabs(z)) + I * carg(z);
}
float complex clogf(float complex z) { return (float complex)clog((double complex)z); }
long double complex clogl(long double complex z) { return (long double complex)clog((double complex)z); }

double complex cpow(double complex x, double complex y) {
  return cexp(y * clog(x));
}
float complex cpowf(float complex x, float complex y) { return (float complex)cpow((double complex)x, (double complex)y); }
long double complex cpowl(long double complex x, long double complex y) { return (long double complex)cpow((double complex)x, (double complex)y); }

double complex csqrt(double complex z) {
  double x = creal(z);
  double y = cimag(z);
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
  return sin(x) * cosh(y) + I * (cos(x) * sinh(y));
}
float complex csinf(float complex z) { return (float complex)csin((double complex)z); }
long double complex csinl(long double complex z) { return (long double complex)csin((double complex)z); }

double complex ccos(double complex z) {
  double x = creal(z), y = cimag(z);
  return cos(x) * cosh(y) - I * (sin(x) * sinh(y));
}
float complex ccosf(float complex z) { return (float complex)ccos((double complex)z); }
long double complex ccosl(long double complex z) { return (long double complex)ccos((double complex)z); }

double complex ctan(double complex z) { return csin(z) / ccos(z); }
float complex ctanf(float complex z) { return (float complex)ctan((double complex)z); }
long double complex ctanl(long double complex z) { return (long double complex)ctan((double complex)z); }

double complex csinh(double complex z) {
  double x = creal(z), y = cimag(z);
  return sinh(x) * cos(y) + I * (cosh(x) * sin(y));
}
float complex csinhf(float complex z) { return (float complex)csinh((double complex)z); }
long double complex csinhl(long double complex z) { return (long double complex)csinh((double complex)z); }

double complex ccosh(double complex z) {
  double x = creal(z), y = cimag(z);
  return cosh(x) * cos(y) + I * (sinh(x) * sin(y));
}
float complex ccoshf(float complex z) { return (float complex)ccosh((double complex)z); }
long double complex ccoshl(long double complex z) { return (long double complex)ccosh((double complex)z); }

double complex ctanh(double complex z) { return csinh(z) / ccosh(z); }
float complex ctanhf(float complex z) { return (float complex)ctanh((double complex)z); }
long double complex ctanhl(long double complex z) { return (long double complex)ctanh((double complex)z); }

double complex casin(double complex z) {
  double complex iz = I * z;
  return -I * clog(iz + csqrt(1.0 - z * z));
}
float complex casinf(float complex z) { return (float complex)casin((double complex)z); }
long double complex casinl(long double complex z) { return (long double complex)casin((double complex)z); }

double complex cacos(double complex z) {
  return M_PI_2 - casin(z);
}
float complex cacosf(float complex z) { return (float complex)cacos((double complex)z); }
long double complex cacosl(long double complex z) { return (long double complex)cacos((double complex)z); }

double complex catan(double complex z) {
  return (I * 0.5) * (clog(1.0 - I * z) - clog(1.0 + I * z));
}
float complex catanf(float complex z) { return (float complex)catan((double complex)z); }
long double complex catanl(long double complex z) { return (long double complex)catan((double complex)z); }

double complex casinh(double complex z) {
  return clog(z + csqrt(z * z + 1.0));
}
float complex casinhf(float complex z) { return (float complex)casinh((double complex)z); }
long double complex casinhl(long double complex z) { return (long double complex)casinh((double complex)z); }

double complex cacosh(double complex z) {
  return clog(z + csqrt(z + 1.0) * csqrt(z - 1.0));
}
float complex cacoshf(float complex z) { return (float complex)cacosh((double complex)z); }
long double complex cacoshl(long double complex z) { return (long double complex)cacosh((double complex)z); }

double complex catanh(double complex z) {
  return 0.5 * (clog(1.0 + z) - clog(1.0 - z));
}
float complex catanhf(float complex z) { return (float complex)catanh((double complex)z); }
long double complex catanhl(long double complex z) { return (long double complex)catanh((double complex)z); }
