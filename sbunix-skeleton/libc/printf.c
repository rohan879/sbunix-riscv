#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
 * vsnprintf / snprintf — core formatting engine
 *
 * Supports: %d %i %u %x %X %o %p %s %c %%
 * Flags:    - (left-align)  0 (zero-pad)
 * Width:    numeric field width
 * Length:   l (long), ll (long long), z (size_t), h (short)
 * ---------------------------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t size;
    size_t pos;
} snp_ctx_t;

static void snp_emit(snp_ctx_t *ctx, char c) {
    if (ctx->buf && ctx->pos + 1 < ctx->size)
        ctx->buf[ctx->pos] = c;
    ctx->pos++;
}

static void snp_pad(snp_ctx_t *ctx, int n, char ch) {
    while (n-- > 0)
        snp_emit(ctx, ch);
}

/* Format an unsigned integer into tmp[], return length (no NUL). */
static int fmt_uint(char *tmp, int tmpsz, unsigned long long val,
                    int base, int upper) {
    const char *lo = "0123456789abcdef";
    const char *up = "0123456789ABCDEF";
    const char *digits = upper ? up : lo;
    int i = 0;
    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val && i < tmpsz - 1) {
            tmp[i++] = digits[val % (unsigned)base];
            val /= (unsigned)base;
        }
        /* reverse */
        for (int a = 0, b = i - 1; a < b; a++, b--) {
            char t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t;
        }
    }
    tmp[i] = '\0';
    return i;
}

static void snp_emit_str_padded(snp_ctx_t *ctx, const char *s,
                                 int width, int left_align) {
    int slen = strlen(s);
    int pad  = width - slen;
    if (!left_align && pad > 0)
        snp_pad(ctx, pad, ' ');
    while (*s)
        snp_emit(ctx, *s++);
    if (left_align && pad > 0)
        snp_pad(ctx, pad, ' ');
}

/* ENH: snp_emit_num extended with sign_plus and sign_space flags to
 * support the '+' and ' ' format flags for signed integer output. */
static void snp_emit_num(snp_ctx_t *ctx,
                          unsigned long long uval, int is_neg,
                          int base, int upper,
                          int width, int zero_pad, int left_align,
                          int alt, int sign_plus, int sign_space) {
    char tmp[32];
    char prefix[4];
    int  plen = 0;

    if (is_neg)
        prefix[plen++] = '-';
    else if (sign_plus)
        prefix[plen++] = '+';
    else if (sign_space)
        prefix[plen++] = ' ';

    if (alt && base == 16) {
        prefix[plen++] = '0';
        prefix[plen++] = upper ? 'X' : 'x';
    }
    prefix[plen] = '\0';

    int nlen = fmt_uint(tmp, sizeof(tmp), uval, base, upper);
    int total = plen + nlen;
    int pad   = width - total;
    char padch = (zero_pad && !left_align) ? '0' : ' ';

    if (!left_align && pad > 0 && padch == ' ')
        snp_pad(ctx, pad, ' ');
    /* prefix (sign or 0x) */
    for (int i = 0; i < plen; i++)
        snp_emit(ctx, prefix[i]);
    if (!left_align && pad > 0 && padch == '0')
        snp_pad(ctx, pad, '0');
    /* digits */
    for (int i = 0; i < nlen; i++)
        snp_emit(ctx, tmp[i]);
    if (left_align && pad > 0)
        snp_pad(ctx, pad, ' ');
}

/* -----------------------------------------------------------------------
 * Floating-point formatting helpers
 *
 * ENH: Added %f/%F/%e/%E/%g/%G formatting with infinity/NaN support,
 * sign flags (+/space), and precision handling.  Uses integer decomposition
 * (no libm dependency beyond isnan/isinf from math.h).
 * ---------------------------------------------------------------------- */

/* Helper: emit the string representation of a double in %f format.
 * Handles sign, inf, nan, integer and fractional parts with precision. */
static void snp_emit_float_f(snp_ctx_t *ctx, double val, int width,
                              int prec, int zero_pad, int left_align,
                              int sign_plus, int sign_space, int alt,
                              int upper) {
  /* Handle NaN */
  if (val != val) { /* NaN check: NaN != NaN */
    const char *s = upper ? "NAN" : "nan";
    int slen = 3;
    int pad = width - slen;
    if (!left_align && pad > 0) snp_pad(ctx, pad, ' ');
    for (int i = 0; i < slen; i++) snp_emit(ctx, s[i]);
    if (left_align && pad > 0) snp_pad(ctx, pad, ' ');
    return;
  }

  /* Determine sign */
  int neg = 0;
  union { double d; unsigned long long u; } ub;
  ub.d = val;
  if (ub.u >> 63) { neg = 1; val = -val; }

  /* Handle infinity */
  /* Infinity check: val > 1e308 is a crude but portable check */
  union { double d; unsigned long long u; } ic;
  ic.d = val;
  int is_inf = ((ic.u >> 52) & 0x7ff) == 0x7ff && (ic.u & 0x000fffffffffffffULL) == 0;
  if (is_inf) {
    const char *s = upper ? "INF" : "inf";
    char sign_ch = 0;
    if (neg) sign_ch = '-';
    else if (sign_plus) sign_ch = '+';
    else if (sign_space) sign_ch = ' ';
    int slen = 3 + (sign_ch ? 1 : 0);
    int pad = width - slen;
    if (!left_align && pad > 0) snp_pad(ctx, pad, ' ');
    if (sign_ch) snp_emit(ctx, sign_ch);
    for (int i = 0; i < 3; i++) snp_emit(ctx, s[i]);
    if (left_align && pad > 0) snp_pad(ctx, pad, ' ');
    return;
  }

  if (prec < 0) prec = 6;

  /* Build the formatted number into a temporary buffer.
   * Max: sign(1) + integer digits(~20) + '.'(1) + frac digits(prec) + NUL */
  char buf[350];
  int pos = 0;

  /* Sign character */
  if (neg) buf[pos++] = '-';
  else if (sign_plus) buf[pos++] = '+';
  else if (sign_space) buf[pos++] = ' ';

  /* Decompose into integer and fractional parts */
  /* Round: add 0.5 * 10^(-prec) */
  double rounding = 0.5;
  for (int i = 0; i < prec; i++) rounding /= 10.0;
  val += rounding;

  /* Extract integer part */
  unsigned long long int_part = (unsigned long long)val;
  double frac = val - (double)int_part;

  /* Format integer part */
  char itmp[24];
  int ilen = fmt_uint(itmp, sizeof(itmp), int_part, 10, 0);
  for (int i = 0; i < ilen; i++) buf[pos++] = itmp[i];

  /* Decimal point and fractional digits */
  if (prec > 0 || alt) {
    buf[pos++] = '.';
    for (int i = 0; i < prec && pos < (int)sizeof(buf) - 1; i++) {
      frac *= 10.0;
      int d = (int)frac;
      if (d > 9) d = 9;
      buf[pos++] = '0' + d;
      frac -= d;
    }
  }
  buf[pos] = '\0';

  /* Now emit with padding */
  int pad = width - pos;
  char padch = (zero_pad && !left_align) ? '0' : ' ';

  if (!left_align && pad > 0 && padch == ' ')
    snp_pad(ctx, pad, ' ');
  /* If zero-padding, emit sign first, then zeros, then digits */
  if (!left_align && pad > 0 && padch == '0') {
    int si = 0;
    if (pos > 0 && (buf[0] == '-' || buf[0] == '+' || buf[0] == ' ')) {
      snp_emit(ctx, buf[0]);
      si = 1;
    }
    snp_pad(ctx, pad, '0');
    for (int i = si; i < pos; i++) snp_emit(ctx, buf[i]);
  } else {
    for (int i = 0; i < pos; i++) snp_emit(ctx, buf[i]);
  }
  if (left_align && pad > 0)
    snp_pad(ctx, pad, ' ');
}

/* Helper: emit float in %e format (scientific notation) */
static void snp_emit_float_e(snp_ctx_t *ctx, double val, int width,
                              int prec, int zero_pad, int left_align,
                              int sign_plus, int sign_space, int alt,
                              int upper) {
  /* Handle NaN */
  if (val != val) {
    const char *s = upper ? "NAN" : "nan";
    int slen = 3;
    int pad = width - slen;
    if (!left_align && pad > 0) snp_pad(ctx, pad, ' ');
    for (int i = 0; i < slen; i++) snp_emit(ctx, s[i]);
    if (left_align && pad > 0) snp_pad(ctx, pad, ' ');
    return;
  }

  int neg = 0;
  union { double d; unsigned long long u; } ub;
  ub.d = val;
  if (ub.u >> 63) { neg = 1; val = -val; }

  /* Check infinity */
  union { double d; unsigned long long u; } ic;
  ic.d = val;
  int is_inf = ((ic.u >> 52) & 0x7ff) == 0x7ff && (ic.u & 0x000fffffffffffffULL) == 0;
  if (is_inf) {
    const char *s = upper ? "INF" : "inf";
    char sign_ch = 0;
    if (neg) sign_ch = '-';
    else if (sign_plus) sign_ch = '+';
    else if (sign_space) sign_ch = ' ';
    int slen = 3 + (sign_ch ? 1 : 0);
    int pad = width - slen;
    if (!left_align && pad > 0) snp_pad(ctx, pad, ' ');
    if (sign_ch) snp_emit(ctx, sign_ch);
    for (int i = 0; i < 3; i++) snp_emit(ctx, s[i]);
    if (left_align && pad > 0) snp_pad(ctx, pad, ' ');
    return;
  }

  if (prec < 0) prec = 6;

  /* Normalize: find exponent so that 1.0 <= val < 10.0 */
  int exponent = 0;
  if (val == 0.0) {
    exponent = 0;
  } else {
    /* Scale val into [1.0, 10.0) */
    while (val >= 10.0) { val /= 10.0; exponent++; }
    while (val < 1.0 && val > 0.0) { val *= 10.0; exponent--; }
  }

  /* Now format as: d.ddddddde+XX */
  char buf[350];
  int pos = 0;

  if (neg) buf[pos++] = '-';
  else if (sign_plus) buf[pos++] = '+';
  else if (sign_space) buf[pos++] = ' ';

  /* Round */
  double rounding = 0.5;
  for (int i = 0; i < prec; i++) rounding /= 10.0;
  val += rounding;
  if (val >= 10.0) { val /= 10.0; exponent++; }

  /* Integer digit */
  int d = (int)val;
  if (d > 9) d = 9;
  buf[pos++] = '0' + d;
  double frac = val - d;

  if (prec > 0 || alt) {
    buf[pos++] = '.';
    for (int i = 0; i < prec && pos < (int)sizeof(buf) - 10; i++) {
      frac *= 10.0;
      int fd = (int)frac;
      if (fd > 9) fd = 9;
      buf[pos++] = '0' + fd;
      frac -= fd;
    }
  }

  /* Exponent: e+XX or e-XX */
  buf[pos++] = upper ? 'E' : 'e';
  buf[pos++] = (exponent >= 0) ? '+' : '-';
  int aexp = (exponent < 0) ? -exponent : exponent;
  /* At least 2 digits */
  if (aexp < 10) {
    buf[pos++] = '0';
    buf[pos++] = '0' + aexp;
  } else if (aexp < 100) {
    buf[pos++] = '0' + aexp / 10;
    buf[pos++] = '0' + aexp % 10;
  } else {
    buf[pos++] = '0' + aexp / 100;
    buf[pos++] = '0' + (aexp / 10) % 10;
    buf[pos++] = '0' + aexp % 10;
  }
  buf[pos] = '\0';

  /* Emit with padding */
  int pad = width - pos;
  char padch = (zero_pad && !left_align) ? '0' : ' ';

  if (!left_align && pad > 0 && padch == ' ')
    snp_pad(ctx, pad, ' ');
  if (!left_align && pad > 0 && padch == '0') {
    int si = 0;
    if (pos > 0 && (buf[0] == '-' || buf[0] == '+' || buf[0] == ' ')) {
      snp_emit(ctx, buf[0]);
      si = 1;
    }
    snp_pad(ctx, pad, '0');
    for (int i = si; i < pos; i++) snp_emit(ctx, buf[i]);
  } else {
    for (int i = 0; i < pos; i++) snp_emit(ctx, buf[i]);
  }
  if (left_align && pad > 0)
    snp_pad(ctx, pad, ' ');
}

/* Helper: compute base-10 exponent for %g format selection */
static int float_exponent(double val) {
  if (val == 0.0) return 0;
  int e = 0;
  while (val >= 10.0) { val /= 10.0; e++; }
  while (val < 1.0 && val > 0.0) { val *= 10.0; e--; }
  return e;
}

/* Helper: emit float in %g/%G format -- choose %e or %f style and
 * strip trailing zeros (unless # flag). */
static void snp_emit_float_g(snp_ctx_t *ctx, double val, int width,
                              int prec, int zero_pad, int left_align,
                              int sign_plus, int sign_space, int alt,
                              int upper) {
  /* Handle NaN */
  if (val != val) {
    const char *s = upper ? "NAN" : "nan";
    int slen = 3;
    int pad = width - slen;
    if (!left_align && pad > 0) snp_pad(ctx, pad, ' ');
    for (int i = 0; i < slen; i++) snp_emit(ctx, s[i]);
    if (left_align && pad > 0) snp_pad(ctx, pad, ' ');
    return;
  }

  int neg = 0;
  union { double d; unsigned long long u; } ub;
  ub.d = val;
  if (ub.u >> 63) { neg = 1; val = -val; }

  /* Check infinity */
  union { double d; unsigned long long u; } ic;
  ic.d = val;
  int is_inf = ((ic.u >> 52) & 0x7ff) == 0x7ff && (ic.u & 0x000fffffffffffffULL) == 0;
  if (is_inf) {
    const char *s = upper ? "INF" : "inf";
    char sign_ch = 0;
    if (neg) sign_ch = '-';
    else if (sign_plus) sign_ch = '+';
    else if (sign_space) sign_ch = ' ';
    int slen = 3 + (sign_ch ? 1 : 0);
    int pad = width - slen;
    if (!left_align && pad > 0) snp_pad(ctx, pad, ' ');
    if (sign_ch) snp_emit(ctx, sign_ch);
    for (int i = 0; i < 3; i++) snp_emit(ctx, s[i]);
    if (left_align && pad > 0) snp_pad(ctx, pad, ' ');
    return;
  }

  if (prec < 0) prec = 6;
  if (prec == 0) prec = 1;

  /* %g rule: use %e if exponent < -4 or >= prec, else %f.
   * prec specifies total significant digits (not fractional digits). */
  int e = float_exponent(val);

  /* Re-negate for the helper calls */
  if (neg) val = -val;

  if (e < -4 || e >= prec) {
    /* Use %e style with (prec-1) fractional digits */
    int fprec = prec - 1;
    if (fprec < 0) fprec = 0;

    /* Format into a temporary buffer, then strip trailing zeros */
    snp_ctx_t tmp_ctx;
    char tmp_buf[350];
    tmp_ctx.buf = tmp_buf;
    tmp_ctx.size = sizeof(tmp_buf);
    tmp_ctx.pos = 0;

    snp_emit_float_e(&tmp_ctx, val, 0, fprec, 0, 0,
                     sign_plus, sign_space, 1, upper);
    if (tmp_ctx.buf && tmp_ctx.pos < tmp_ctx.size)
      tmp_ctx.buf[tmp_ctx.pos] = '\0';

    /* Strip trailing zeros from fractional part (unless # flag) */
    if (!alt) {
      /* Find 'e' or 'E' */
      int epos = -1;
      for (int i = 0; i < (int)tmp_ctx.pos; i++) {
        if (tmp_buf[i] == 'e' || tmp_buf[i] == 'E') { epos = i; break; }
      }
      if (epos > 0) {
        /* Strip trailing zeros before exponent */
        int strip_end = epos;
        while (strip_end > 0 && tmp_buf[strip_end - 1] == '0') strip_end--;
        if (strip_end > 0 && tmp_buf[strip_end - 1] == '.') strip_end--;
        /* Move exponent part */
        int elen = (int)tmp_ctx.pos - epos;
        for (int i = 0; i < elen; i++) tmp_buf[strip_end + i] = tmp_buf[epos + i];
        tmp_ctx.pos = strip_end + elen;
        tmp_buf[tmp_ctx.pos] = '\0';
      }
    }

    /* Emit with width padding */
    int slen = (int)tmp_ctx.pos;
    int pad = width - slen;
    if (!left_align && pad > 0) snp_pad(ctx, pad, zero_pad ? '0' : ' ');
    for (int i = 0; i < slen; i++) snp_emit(ctx, tmp_buf[i]);
    if (left_align && pad > 0) snp_pad(ctx, pad, ' ');
  } else {
    /* Use %f style with (prec - 1 - e) fractional digits
     * (prec total significant digits, e+1 before decimal) */
    int fprec = prec - 1 - e;
    if (fprec < 0) fprec = 0;

    /* Format into a temporary buffer, then strip trailing zeros */
    snp_ctx_t tmp_ctx;
    char tmp_buf[350];
    tmp_ctx.buf = tmp_buf;
    tmp_ctx.size = sizeof(tmp_buf);
    tmp_ctx.pos = 0;

    snp_emit_float_f(&tmp_ctx, val, 0, fprec, 0, 0,
                     sign_plus, sign_space, 1, upper);
    if (tmp_ctx.buf && tmp_ctx.pos < tmp_ctx.size)
      tmp_ctx.buf[tmp_ctx.pos] = '\0';

    /* Strip trailing zeros (unless # flag) */
    if (!alt) {
      while (tmp_ctx.pos > 0 && tmp_buf[tmp_ctx.pos - 1] == '0')
        tmp_ctx.pos--;
      if (tmp_ctx.pos > 0 && tmp_buf[tmp_ctx.pos - 1] == '.')
        tmp_ctx.pos--;
      tmp_buf[tmp_ctx.pos] = '\0';
    }

    /* Emit with width padding */
    int slen = (int)tmp_ctx.pos;
    int pad = width - slen;
    if (!left_align && pad > 0) snp_pad(ctx, pad, zero_pad ? '0' : ' ');
    for (int i = 0; i < slen; i++) snp_emit(ctx, tmp_buf[i]);
    if (left_align && pad > 0) snp_pad(ctx, pad, ' ');
  }
}

/* -----------------------------------------------------------------------
 * Positional argument support (%N$)
 *
 * ENH: Added two-pass positional argument extraction so format strings
 * like "%2$s %1$d" work correctly.  Pass 1 scans the format to catalog
 * each argument's type by position.  Pass 2 extracts all args into a
 * union array, then formats using positional indices.
 * ---------------------------------------------------------------------- */

/* Union for positional argument storage */
union printarg {
  int i; unsigned u; long l; unsigned long ul;
  long long ll; unsigned long long ull;
  double d; void *p; char *s;
};

enum argtype {
  PA_INT = 0, PA_UINT, PA_LONG, PA_ULONG, PA_LLONG, PA_ULLONG,
  PA_DOUBLE, PA_PTR, PA_STRING, PA_CHAR, PA_SSIZE, PA_SIZE,
  PA_INTP, PA_LONGP, PA_LLONGP
};

#define MAX_POS_ARGS 32

/* Scan format string to detect if positional args are used and catalog types.
 * Returns 1 if positional args detected, 0 otherwise. */
static int scan_positional(const char *fmt, enum argtype *types, int *max_pos) {
  int found_positional = 0;
  *max_pos = 0;

  const char *p = fmt;
  while (*p) {
    if (*p != '%') { p++; continue; }
    p++; /* skip % */
    if (*p == '%') { p++; continue; }

    /* Check for N$ positional indicator */
    const char *save = p;
    int pos_idx = 0;
    while (*p >= '0' && *p <= '9')
      pos_idx = pos_idx * 10 + (*p++ - '0');
    if (*p == '$' && pos_idx > 0) {
      found_positional = 1;
      p++; /* skip $ */
    } else {
      /* Not positional, reset */
      p = save;
      pos_idx = 0;
    }

    /* Skip flags */
    while (*p == '-' || *p == '0' || *p == '#' || *p == '+' || *p == ' ') p++;

    /* Skip width (may be * or *N$) */
    if (*p == '*') {
      p++;
      /* Could be *N$ for positional width */
      const char *ws = p;
      int wpos = 0;
      while (*p >= '0' && *p <= '9')
        wpos = wpos * 10 + (*p++ - '0');
      if (*p == '$' && wpos > 0) {
        p++;
        if (wpos <= MAX_POS_ARGS && wpos > *max_pos) *max_pos = wpos;
        if (wpos <= MAX_POS_ARGS) types[wpos - 1] = PA_INT;
      } else {
        p = ws; /* not positional width, just * */
      }
    } else {
      while (*p >= '0' && *p <= '9') p++;
    }

    /* Skip precision */
    if (*p == '.') {
      p++;
      if (*p == '*') {
        p++;
        const char *ps = p;
        int ppos = 0;
        while (*p >= '0' && *p <= '9')
          ppos = ppos * 10 + (*p++ - '0');
        if (*p == '$' && ppos > 0) {
          p++;
          if (ppos <= MAX_POS_ARGS && ppos > *max_pos) *max_pos = ppos;
          if (ppos <= MAX_POS_ARGS) types[ppos - 1] = PA_INT;
        } else {
          p = ps;
        }
      } else {
        if (*p == '-') p++; /* negative precision */
        while (*p >= '0' && *p <= '9') p++;
      }
    }

    /* Length modifier */
    int is_long = 0, is_longlong = 0, is_size = 0;
    if (*p == 'l') { p++; if (*p == 'l') { is_longlong = 1; p++; } else is_long = 1; }
    else if (*p == 'h') { p++; if (*p == 'h') p++; }
    else if (*p == 'z') { is_size = 1; p++; }
    else if (*p == 'L') { p++; is_long = 2; /* long double marker */ }
    (void)is_longlong; (void)is_size; /* used in type classification below */

    if (pos_idx > 0 && pos_idx <= MAX_POS_ARGS) {
      if (pos_idx > *max_pos) *max_pos = pos_idx;
      switch (*p) {
      case 'd': case 'i':
        if (is_longlong) types[pos_idx - 1] = PA_LLONG;
        else if (is_long) types[pos_idx - 1] = PA_LONG;
        else if (is_size) types[pos_idx - 1] = PA_SSIZE;
        else types[pos_idx - 1] = PA_INT;
        break;
      case 'u':
        if (is_longlong) types[pos_idx - 1] = PA_ULLONG;
        else if (is_long) types[pos_idx - 1] = PA_ULONG;
        else if (is_size) types[pos_idx - 1] = PA_SIZE;
        else types[pos_idx - 1] = PA_UINT;
        break;
      case 'x': case 'X':
        if (is_longlong) types[pos_idx - 1] = PA_ULLONG;
        else if (is_long) types[pos_idx - 1] = PA_ULONG;
        else types[pos_idx - 1] = PA_UINT;
        break;
      case 'o':
        if (is_long) types[pos_idx - 1] = PA_ULONG;
        else types[pos_idx - 1] = PA_UINT;
        break;
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
        types[pos_idx - 1] = PA_DOUBLE;
        break;
      case 'p':
        types[pos_idx - 1] = PA_PTR;
        break;
      case 's':
        types[pos_idx - 1] = PA_STRING;
        break;
      case 'c':
        types[pos_idx - 1] = PA_CHAR;
        break;
      case 'n':
        if (is_longlong) types[pos_idx - 1] = PA_LLONGP;
        else if (is_long) types[pos_idx - 1] = PA_LONGP;
        else types[pos_idx - 1] = PA_INTP;
        break;
      default: break;
      }
    }
    if (*p) p++;
  }
  return found_positional;
}

/* Extract positional args from va_list into array */
static void extract_args(va_list ap, enum argtype *types, union printarg *args, int count) {
  for (int i = 0; i < count; i++) {
    switch (types[i]) {
    case PA_INT: case PA_CHAR:
      args[i].i = va_arg(ap, int); break;
    case PA_UINT:
      args[i].u = va_arg(ap, unsigned int); break;
    case PA_LONG:
      args[i].l = va_arg(ap, long); break;
    case PA_ULONG:
      args[i].ul = va_arg(ap, unsigned long); break;
    case PA_LLONG:
      args[i].ll = va_arg(ap, long long); break;
    case PA_ULLONG:
      args[i].ull = va_arg(ap, unsigned long long); break;
    case PA_SSIZE:
      args[i].l = (long)va_arg(ap, ssize_t); break;
    case PA_SIZE:
      args[i].ul = (unsigned long)va_arg(ap, size_t); break;
    case PA_DOUBLE:
      args[i].d = va_arg(ap, double); break;
    case PA_PTR:
      args[i].p = va_arg(ap, void *); break;
    case PA_STRING:
      args[i].s = va_arg(ap, char *); break;
    case PA_INTP:
      args[i].p = va_arg(ap, int *); break;
    case PA_LONGP:
      args[i].p = va_arg(ap, long *); break;
    case PA_LLONGP:
      args[i].p = va_arg(ap, long long *); break;
    }
  }
}

/* ENH: do_vsnprintf rewritten to support:
 * - '+' and ' ' sign flags for signed conversions
 * - '*' width and precision from va_args
 * - %f/%F/%e/%E/%g/%G floating-point formatting with inf/nan
 * - %N$ positional argument references
 * - %L (long double, treated as double on this platform) */
static void do_vsnprintf(snp_ctx_t *ctx, const char *fmt, va_list ap) {
    /* Check for positional args */
    enum argtype pos_types[MAX_POS_ARGS];
    union printarg pos_args[MAX_POS_ARGS];
    int max_pos = 0;
    int use_positional = 0;

    for (int i = 0; i < MAX_POS_ARGS; i++) pos_types[i] = PA_INT;

    {
      va_list ap_scan;
      va_copy(ap_scan, ap);
      use_positional = scan_positional(fmt, pos_types, &max_pos);
      va_end(ap_scan);
    }

    if (use_positional && max_pos > 0) {
      va_list ap_extract;
      va_copy(ap_extract, ap);
      extract_args(ap_extract, pos_types, pos_args, max_pos);
      va_end(ap_extract);
    }

    while (*fmt) {
        if (*fmt != '%') {
            snp_emit(ctx, *fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* Check for positional arg: N$ */
        int pos_idx = 0;
        {
          const char *save = fmt;
          int idx = 0;
          while (*fmt >= '0' && *fmt <= '9')
            idx = idx * 10 + (*fmt++ - '0');
          if (*fmt == '$' && idx > 0) {
            pos_idx = idx;
            fmt++; /* skip '$' */
          } else {
            fmt = save;
          }
        }

        /* Flags -- ENH: added '+' and ' ' (space) flag parsing */
        int left_align = 0, zero_pad = 0, alt = 0;
        int sign_plus = 0, sign_space = 0;
        for (;;) {
            if (*fmt == '-')      { left_align = 1; fmt++; }
            else if (*fmt == '0') { zero_pad   = 1; fmt++; }
            else if (*fmt == '#') { alt        = 1; fmt++; }
            else if (*fmt == '+') { sign_plus  = 1; fmt++; }
            else if (*fmt == ' ') { sign_space = 1; fmt++; }
            else break;
        }
        /* '+' takes precedence over ' ' */
        if (sign_plus) sign_space = 0;

        /* Width -- ENH: added '*' width from va_args and *N$ positional */
        int width = 0;
        if (*fmt == '*') {
            fmt++;
            /* Check for *N$ */
            const char *ws = fmt;
            int wpos = 0;
            while (*fmt >= '0' && *fmt <= '9')
              wpos = wpos * 10 + (*fmt++ - '0');
            if (*fmt == '$' && wpos > 0) {
              fmt++;
              if (wpos <= max_pos) width = pos_args[wpos - 1].i;
            } else {
              fmt = ws;
              if (!use_positional)
                width = va_arg(ap, int);
            }
            if (width < 0) { left_align = 1; width = -width; }
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }

        /* Precision -- ENH: added '*' precision and negative precision */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                fmt++;
                /* Check for *N$ */
                const char *ps = fmt;
                int ppos = 0;
                while (*fmt >= '0' && *fmt <= '9')
                  ppos = ppos * 10 + (*fmt++ - '0');
                if (*fmt == '$' && ppos > 0) {
                  fmt++;
                  if (ppos <= max_pos) prec = pos_args[ppos - 1].i;
                } else {
                  fmt = ps;
                  if (!use_positional)
                    prec = va_arg(ap, int);
                }
                if (prec < 0) prec = -1; /* negative precision = no precision */
            } else if (*fmt == '-') {
                /* Negative precision: treat as if precision were omitted */
                fmt++;
                while (*fmt >= '0' && *fmt <= '9') fmt++;
                prec = -1;
            } else {
                prec = 0;
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }
        /* Length modifier -- ENH: added 'L' for long double (treated as double) */
        int is_long = 0, is_longlong = 0, is_short = 0, is_size = 0;
        int is_longdouble = 0;
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') { is_longlong = 1; fmt++; }
            else               is_long = 1;
        } else if (*fmt == 'h') {
            is_short = 1; fmt++;
            if (*fmt == 'h') fmt++; /* hh */
        } else if (*fmt == 'z') {
            is_size = 1; fmt++;
        } else if (*fmt == 'L') {
            is_longdouble = 1; fmt++;
        } else if (*fmt == 'j') {
            is_longlong = 1; fmt++; /* intmax_t = long long */
        } else if (*fmt == 't') {
            is_long = 1; fmt++; /* ptrdiff_t = long */
        }

        /* Get argument value for current specifier */
        switch (*fmt) {
        case 'd': case 'i': {
            long long sv;
            if (pos_idx > 0 && pos_idx <= max_pos) {
                enum argtype t = pos_types[pos_idx - 1];
                if (t == PA_LLONG) sv = pos_args[pos_idx - 1].ll;
                else if (t == PA_LONG || t == PA_SSIZE) sv = pos_args[pos_idx - 1].l;
                else sv = pos_args[pos_idx - 1].i;
            } else {
                if (is_longlong)     sv = (long long)va_arg(ap, long long);
                else if (is_long)    sv = (long long)va_arg(ap, long);
                else if (is_size)    sv = (long long)va_arg(ap, ssize_t);
                else if (is_short)   sv = (long long)(short)va_arg(ap, int);
                else                 sv = (long long)va_arg(ap, int);
            }
            int neg = (sv < 0);
            unsigned long long uv;
            if (neg) {
                uv = (unsigned long long)(~sv) + 1ULL;
            } else {
                uv = (unsigned long long)sv;
            }
            snp_emit_num(ctx, uv, neg, 10, 0, width, zero_pad, left_align, 0,
                         sign_plus, sign_space);
            break;
        }
        case 'u': {
            unsigned long long uv;
            if (pos_idx > 0 && pos_idx <= max_pos) {
                enum argtype t = pos_types[pos_idx - 1];
                if (t == PA_ULLONG) uv = pos_args[pos_idx - 1].ull;
                else if (t == PA_ULONG || t == PA_SIZE) uv = pos_args[pos_idx - 1].ul;
                else uv = (unsigned long long)pos_args[pos_idx - 1].u;
            } else {
                if (is_longlong)   uv = va_arg(ap, unsigned long long);
                else if (is_long)  uv = va_arg(ap, unsigned long);
                else if (is_size)  uv = va_arg(ap, size_t);
                else               uv = (unsigned long long)va_arg(ap, unsigned int);
            }
            snp_emit_num(ctx, uv, 0, 10, 0, width, zero_pad, left_align, 0, 0, 0);
            break;
        }
        case 'x': case 'X': {
            unsigned long long uv;
            if (pos_idx > 0 && pos_idx <= max_pos) {
                enum argtype t = pos_types[pos_idx - 1];
                if (t == PA_ULLONG) uv = pos_args[pos_idx - 1].ull;
                else if (t == PA_ULONG) uv = pos_args[pos_idx - 1].ul;
                else uv = (unsigned long long)pos_args[pos_idx - 1].u;
            } else {
                if (is_longlong)   uv = va_arg(ap, unsigned long long);
                else if (is_long)  uv = va_arg(ap, unsigned long);
                else               uv = (unsigned long long)va_arg(ap, unsigned int);
            }
            snp_emit_num(ctx, uv, 0, 16, (*fmt == 'X'),
                         width, zero_pad, left_align, alt, 0, 0);
            break;
        }
        case 'o': {
            unsigned long long uv;
            if (pos_idx > 0 && pos_idx <= max_pos) {
                enum argtype t = pos_types[pos_idx - 1];
                if (t == PA_ULONG) uv = pos_args[pos_idx - 1].ul;
                else uv = (unsigned long long)pos_args[pos_idx - 1].u;
            } else {
                if (is_long)  uv = va_arg(ap, unsigned long);
                else          uv = (unsigned long long)va_arg(ap, unsigned int);
            }
            snp_emit_num(ctx, uv, 0, 8, 0, width, zero_pad, left_align, 0, 0, 0);
            break;
        }
        case 'p': {
            unsigned long long uv;
            if (pos_idx > 0 && pos_idx <= max_pos) {
                uv = (unsigned long long)(unsigned long)pos_args[pos_idx - 1].p;
            } else {
                uv = (unsigned long long)(unsigned long)va_arg(ap, void *);
            }
            char tmp[32];
            fmt_uint(tmp, sizeof(tmp), uv, 16, 0);
            int tl = strlen(tmp);
            int total = 2 + tl;
            int pad = width - total;
            if (!left_align && pad > 0) snp_pad(ctx, pad, ' ');
            snp_emit(ctx, '0'); snp_emit(ctx, 'x');
            for (int i = 0; i < tl; i++) snp_emit(ctx, tmp[i]);
            if (left_align && pad > 0) snp_pad(ctx, pad, ' ');
            break;
        }
        case 's': {
            const char *s;
            if (pos_idx > 0 && pos_idx <= max_pos) {
                s = pos_args[pos_idx - 1].s;
            } else {
                s = va_arg(ap, const char *);
            }
            if (!s) s = "(null)";
            if (prec >= 0) {
                int slen = 0;
                while (slen < prec && s[slen]) slen++;
                int pad = width - slen;
                if (!left_align && pad > 0) snp_pad(ctx, pad, ' ');
                for (int i = 0; i < slen; i++) snp_emit(ctx, s[i]);
                if (left_align && pad > 0) snp_pad(ctx, pad, ' ');
            } else {
                snp_emit_str_padded(ctx, s, width, left_align);
            }
            break;
        }
        case 'c': {
            char c;
            if (pos_idx > 0 && pos_idx <= max_pos) {
                c = (char)pos_args[pos_idx - 1].i;
            } else {
                c = (char)va_arg(ap, int);
            }
            if (!left_align && width > 1) snp_pad(ctx, width - 1, ' ');
            snp_emit(ctx, c);
            if (left_align  && width > 1) snp_pad(ctx, width - 1, ' ');
            break;
        }
        /* ENH: Added %f/%F floating-point formatting */
        case 'f': case 'F': {
            double dv;
            if (pos_idx > 0 && pos_idx <= max_pos) {
                dv = pos_args[pos_idx - 1].d;
            } else {
                if (is_longdouble) {
                  /* long double promoted to double on this platform */
                  dv = (double)va_arg(ap, long double);
                } else {
                  dv = va_arg(ap, double);
                }
            }
            snp_emit_float_f(ctx, dv, width, prec, zero_pad, left_align,
                             sign_plus, sign_space, alt, (*fmt == 'F'));
            break;
        }
        /* ENH: Added %e/%E scientific notation formatting */
        case 'e': case 'E': {
            double dv;
            if (pos_idx > 0 && pos_idx <= max_pos) {
                dv = pos_args[pos_idx - 1].d;
            } else {
                if (is_longdouble) {
                  dv = (double)va_arg(ap, long double);
                } else {
                  dv = va_arg(ap, double);
                }
            }
            snp_emit_float_e(ctx, dv, width, prec, zero_pad, left_align,
                             sign_plus, sign_space, alt, (*fmt == 'E'));
            break;
        }
        /* ENH: Added %g/%G shortest-representation formatting */
        case 'g': case 'G': {
            double dv;
            if (pos_idx > 0 && pos_idx <= max_pos) {
                dv = pos_args[pos_idx - 1].d;
            } else {
                if (is_longdouble) {
                  dv = (double)va_arg(ap, long double);
                } else {
                  dv = va_arg(ap, double);
                }
            }
            snp_emit_float_g(ctx, dv, width, prec, zero_pad, left_align,
                             sign_plus, sign_space, alt, (*fmt == 'G'));
            break;
        }
        case 'n': {
            if (pos_idx > 0 && pos_idx <= max_pos) {
                /* positional %n -- store into the pointer */
                if (is_longlong) { long long *np = (long long *)pos_args[pos_idx - 1].p; if (np) *np = (long long)ctx->pos; }
                else if (is_long) { long *np = (long *)pos_args[pos_idx - 1].p; if (np) *np = (long)ctx->pos; }
                else { int *np = (int *)pos_args[pos_idx - 1].p; if (np) *np = (int)ctx->pos; }
            } else {
                if (is_longlong) { long long *np = va_arg(ap, long long *); if (np) *np = (long long)ctx->pos; }
                else if (is_long) { long *np = va_arg(ap, long *); if (np) *np = (long)ctx->pos; }
                else { int *np = va_arg(ap, int *); if (np) *np = (int)ctx->pos; }
            }
            break;
        }
        case '%':
            snp_emit(ctx, '%');
            break;
        default:
            snp_emit(ctx, '%');
            if (*fmt) snp_emit(ctx, *fmt);
            break;
        }
        if (*fmt) fmt++;
    }

    /* Always NUL-terminate */
    if (ctx->buf && ctx->size > 0)
        ctx->buf[ctx->pos < ctx->size ? ctx->pos : ctx->size - 1] = '\0';
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    snp_ctx_t ctx = { buf, size, 0 };
    if (size == 0) ctx.buf = (void *)0;
    do_vsnprintf(&ctx, fmt, ap);
    return (int)ctx.pos;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, (size_t)-1, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

/* -----------------------------------------------------------------------
 * vprintf / printf — formatted output to stdout
 * ---------------------------------------------------------------------- */

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

/* -----------------------------------------------------------------------
 * dprintf / vdprintf — formatted output to an arbitrary fd
 * ---------------------------------------------------------------------- */

int vdprintf(int fd, const char *fmt, va_list ap) {
    char tmp[1024];
    int  n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    int  w = n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1;
    write(fd, tmp, w);
    return n;
}

int dprintf(int fd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

/* -----------------------------------------------------------------------
 * Basic I/O helpers
 * ---------------------------------------------------------------------- */

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    if (fputc('\n', stdout) == EOF) return EOF;
    return 0;
}

int putchar(int c) {
    return fputc(c, stdout);
}

int getchar(void) {
    char c;
    if (read(0, &c, 1) == 1)
        return (unsigned char)c;
    return -1;
}
