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

static void snp_emit_num(snp_ctx_t *ctx,
                          unsigned long long uval, int is_neg,
                          int base, int upper,
                          int width, int zero_pad, int left_align,
                          int alt) {
    char tmp[32];
    char prefix[4];
    int  plen = 0;

    if (is_neg)
        prefix[plen++] = '-';
    else if (alt && base == 16) {
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

static void do_vsnprintf(snp_ctx_t *ctx, const char *fmt, va_list ap) {
    while (*fmt) {
        if (*fmt != '%') {
            snp_emit(ctx, *fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* Flags */
        int left_align = 0, zero_pad = 0, alt = 0;
        for (;;) {
            if (*fmt == '-')      { left_align = 1; fmt++; }
            else if (*fmt == '0') { zero_pad   = 1; fmt++; }
            else if (*fmt == '#') { alt        = 1; fmt++; }
            else break;
        }

        /* Width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        /* Precision (consume but mostly ignore for now) */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9')
                prec = prec * 10 + (*fmt++ - '0');
        }
        (void)prec;

        /* Length modifier */
        int is_long = 0, is_longlong = 0, is_short = 0, is_size = 0;
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') { is_longlong = 1; fmt++; }
            else               is_long = 1;
        } else if (*fmt == 'h') {
            is_short = 1; fmt++;
        } else if (*fmt == 'z') {
            is_size = 1; fmt++;
        }

        switch (*fmt) {
        case 'd': case 'i': {
            long long sv;
            if (is_longlong)     sv = (long long)va_arg(ap, long long);
            else if (is_long)    sv = (long long)va_arg(ap, long);
            else if (is_size)    sv = (long long)va_arg(ap, ssize_t);
            else if (is_short)   sv = (long long)(short)va_arg(ap, int);
            else                 sv = (long long)va_arg(ap, int);
            int neg = (sv < 0);
            unsigned long long uv = neg ? -(unsigned long long)sv
                                        : (unsigned long long)sv;
            snp_emit_num(ctx, uv, neg, 10, 0, width, zero_pad, left_align, 0);
            break;
        }
        case 'u': {
            unsigned long long uv;
            if (is_longlong)   uv = va_arg(ap, unsigned long long);
            else if (is_long)  uv = va_arg(ap, unsigned long);
            else if (is_size)  uv = va_arg(ap, size_t);
            else               uv = (unsigned long long)va_arg(ap, unsigned int);
            snp_emit_num(ctx, uv, 0, 10, 0, width, zero_pad, left_align, 0);
            break;
        }
        case 'x': case 'X': {
            unsigned long long uv;
            if (is_longlong)   uv = va_arg(ap, unsigned long long);
            else if (is_long)  uv = va_arg(ap, unsigned long);
            else               uv = (unsigned long long)va_arg(ap, unsigned int);
            snp_emit_num(ctx, uv, 0, 16, (*fmt == 'X'),
                         width, zero_pad, left_align, alt);
            break;
        }
        case 'o': {
            unsigned long long uv;
            if (is_long)  uv = va_arg(ap, unsigned long);
            else          uv = (unsigned long long)va_arg(ap, unsigned int);
            snp_emit_num(ctx, uv, 0, 8, 0, width, zero_pad, left_align, 0);
            break;
        }
        case 'p': {
            unsigned long long uv = (unsigned long long)(unsigned long)va_arg(ap, void *);
            /* always print as 0x... */
            char tmp[32];
            fmt_uint(tmp, sizeof(tmp), uv, 16, 0);
            int tl = strlen(tmp);
            int total = 2 + tl; /* "0x" + digits */
            int pad = width - total;
            if (!left_align && pad > 0) snp_pad(ctx, pad, ' ');
            snp_emit(ctx, '0'); snp_emit(ctx, 'x');
            for (int i = 0; i < tl; i++) snp_emit(ctx, tmp[i]);
            if (left_align && pad > 0) snp_pad(ctx, pad, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            snp_emit_str_padded(ctx, s, width, left_align);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!left_align && width > 1) snp_pad(ctx, width - 1, ' ');
            snp_emit(ctx, c);
            if (left_align  && width > 1) snp_pad(ctx, width - 1, ' ');
            break;
        }
        case '%':
            snp_emit(ctx, '%');
            break;
        default:
            snp_emit(ctx, '%');
            snp_emit(ctx, *fmt);
            break;
        }
        fmt++;
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
    char tmp[4096];
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
