#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int pos;
static int argc;
static char **argv;

static char *peek(void) {
    if (pos < argc)
        return argv[pos];
    return NULL;
}

static char *next(void) {
    if (pos < argc)
        return argv[pos++];
    return NULL;
}

static int is_number(const char *s) {
    if (!s) return 0;
    if (*s == '-' || *s == '+') s++;
    if (*s == '\0') return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        s++;
    }
    return 1;
}

/*
 * Grammar (lowest to highest precedence):
 *   expr    : or_expr
 *   or_expr : and_expr ( '|' and_expr )*
 *   and_expr: cmp_expr ( '&' cmp_expr )*
 *   cmp_expr: add_expr ( ( '=' | '!=' | '<' | '<=' | '>' | '>=' ) add_expr )?
 *   add_expr: mul_expr ( ( '+' | '-' ) mul_expr )*
 *   mul_expr: primary ( ( '*' | '/' | '%' ) primary )*
 *   primary : '(' expr ')' | STRING_OR_NUMBER
 */

/* Forward declarations */
static long or_expr(char *out, int outsz);
static long and_expr(char *out, int outsz);
static long cmp_expr(char *out, int outsz);
static long add_expr(char *out, int outsz);
static long mul_expr(char *out, int outsz);
static long primary(char *out, int outsz);

static long or_expr(char *out, int outsz) {
    long a = and_expr(out, outsz);
    while (peek() && strcmp(peek(), "|") == 0) {
        next();
        char rhs[256];
        long b = and_expr(rhs, sizeof(rhs));
        if (a != 0 || (out[0] != '\0' && strcmp(out, "0") != 0)) {
            /* a is already the result */
        } else {
            a = b;
            strncpy(out, rhs, outsz - 1);
            out[outsz - 1] = '\0';
        }
    }
    return a;
}

static long and_expr(char *out, int outsz) {
    long a = cmp_expr(out, outsz);
    while (peek() && strcmp(peek(), "&") == 0) {
        next();
        char rhs[256];
        long b = cmp_expr(rhs, sizeof(rhs));
        int a_null = (a == 0 && (out[0] == '\0' || strcmp(out, "0") == 0));
        int b_null = (b == 0 && (rhs[0] == '\0' || strcmp(rhs, "0") == 0));
        if (a_null || b_null) {
            a = 0;
            strcpy(out, "0");
        }
    }
    return a;
}

static long cmp_expr(char *out, int outsz) {
    long a = add_expr(out, outsz);
    char *op = peek();
    if (!op) return a;

    if (strcmp(op, "=") == 0 || strcmp(op, "!=") == 0 ||
        strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
        strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) {
        next();
        char rhs[256];
        long b = add_expr(rhs, sizeof(rhs));
        int result;

        /* If both are numbers, compare numerically */
        if (is_number(out) && is_number(rhs)) {
            if (strcmp(op, "=") == 0)       result = (a == b);
            else if (strcmp(op, "!=") == 0)  result = (a != b);
            else if (strcmp(op, "<") == 0)   result = (a < b);
            else if (strcmp(op, "<=") == 0)  result = (a <= b);
            else if (strcmp(op, ">") == 0)   result = (a > b);
            else                             result = (a >= b);
        } else {
            int cmp = strcmp(out, rhs);
            if (strcmp(op, "=") == 0)       result = (cmp == 0);
            else if (strcmp(op, "!=") == 0)  result = (cmp != 0);
            else if (strcmp(op, "<") == 0)   result = (cmp < 0);
            else if (strcmp(op, "<=") == 0)  result = (cmp <= 0);
            else if (strcmp(op, ">") == 0)   result = (cmp > 0);
            else                             result = (cmp >= 0);
        }
        a = result;
        snprintf(out, outsz, "%ld", a);
    }
    return a;
}

static long add_expr(char *out, int outsz) {
    long a = mul_expr(out, outsz);
    while (peek() && (strcmp(peek(), "+") == 0 || strcmp(peek(), "-") == 0)) {
        char *op = next();
        char rhs[256];
        long b = mul_expr(rhs, sizeof(rhs));
        if (strcmp(op, "+") == 0) a += b; else a -= b;
        snprintf(out, outsz, "%ld", a);
    }
    return a;
}

static long mul_expr(char *out, int outsz) {
    long a = primary(out, outsz);
    while (peek() && (strcmp(peek(), "*") == 0 || strcmp(peek(), "/") == 0 ||
                      strcmp(peek(), "%") == 0)) {
        char *op = next();
        char rhs[256];
        long b = primary(rhs, sizeof(rhs));
        if (strcmp(op, "*") == 0)     a *= b;
        else if (strcmp(op, "/") == 0) {
            if (b == 0) { dprintf(2, "expr: division by zero\n"); exit(2); }
            a /= b;
        } else {
            if (b == 0) { dprintf(2, "expr: division by zero\n"); exit(2); }
            a %= b;
        }
        snprintf(out, outsz, "%ld", a);
    }
    return a;
}

static long primary(char *out, int outsz) {
    char *tok = peek();
    if (!tok) {
        dprintf(2, "expr: syntax error\n");
        exit(2);
    }

    if (strcmp(tok, "(") == 0) {
        next();
        long val = or_expr(out, outsz);
        char *cp = next();
        if (!cp || strcmp(cp, ")") != 0) {
            dprintf(2, "expr: missing ')'\n");
            exit(2);
        }
        return val;
    }

    /* Match operator : (pattern matching - simplified) */
    next();
    strncpy(out, tok, outsz - 1);
    out[outsz - 1] = '\0';

    /* Check for : operator (string match) */
    if (peek() && strcmp(peek(), ":") == 0) {
        next();
        char *pat = next();
        if (!pat) { dprintf(2, "expr: syntax error\n"); exit(2); }
        /* Simple prefix match: count matching characters */
        int i = 0;
        while (tok[i] && pat[i] && tok[i] == pat[i])
            i++;
        /* If pattern exhausted, the match length is i */
        long val = (pat[i] == '\0') ? i : 0;
        snprintf(out, outsz, "%ld", val);
        return val;
    }

    if (is_number(tok))
        return atol(tok);
    return 0;
}

int main(int ac, char *av[]) {
    if (ac < 2) {
        dprintf(2, "usage: expr EXPRESSION\n");
        return 2;
    }

    argc = ac;
    argv = av;
    pos = 1;

    char out[256];
    out[0] = '\0';
    long val = or_expr(out, sizeof(out));

    printf("%s\n", out);

    /* expr exits 1 if result is 0 or empty, 0 otherwise */
    if (val == 0 && (out[0] == '\0' || strcmp(out, "0") == 0))
        return 1;
    return 0;
}
