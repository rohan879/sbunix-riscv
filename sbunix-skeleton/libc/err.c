#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

void vwarn(const char *fmt, va_list ap) {
  if (fmt && *fmt) {
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ": %s\n", strerror(errno));
  } else {
    fprintf(stderr, "%s\n", strerror(errno));
  }
}

void warn(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vwarn(fmt, ap);
  va_end(ap);
}

void vwarnx(const char *fmt, va_list ap) {
  if (fmt && *fmt) {
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
  }
}

void warnx(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vwarnx(fmt, ap);
  va_end(ap);
}

void verr(int eval, const char *fmt, va_list ap) {
  vwarn(fmt, ap);
  exit(eval);
}

void err(int eval, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  verr(eval, fmt, ap);
  va_end(ap);
}

void verrx(int eval, const char *fmt, va_list ap) {
  vwarnx(fmt, ap);
  exit(eval);
}

void errx(int eval, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  verrx(eval, fmt, ap);
  va_end(ap);
}
