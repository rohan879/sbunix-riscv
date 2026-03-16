#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

static int argc;
static char **argv;
static int pos;

static int expr(void);

static void error(const char *msg) {
    dprintf(2, "test: %s\n", msg);
}

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

static int primary(void) {
    char *tok = peek();
    if (!tok) return 0;

    /* Negation */
    if (strcmp(tok, "!") == 0) {
        next();
        return !primary();
    }

    /* Parenthesized expression */
    if (strcmp(tok, "(") == 0) {
        next();
        int val = expr();
        char *cp = next();
        if (!cp || strcmp(cp, ")") != 0)
            error("missing ')'");
        return val;
    }

    /* Unary file tests */
    if (tok[0] == '-' && tok[1] != '\0' && tok[2] == '\0') {
        char op = tok[1];
        if (op == 'f' || op == 'd' || op == 'e' || op == 'r' ||
            op == 'w' || op == 'x') {
            next();
            char *path = next();
            if (!path) { error("missing operand"); return 0; }
            struct stat st;
            if (stat(path, &st) < 0) return 0;
            switch (op) {
            case 'e': return 1;
            case 'f': return S_ISREG(st.st_mode);
            case 'd': return S_ISDIR(st.st_mode);
            case 'r': return access(path, R_OK) == 0;
            case 'w': return access(path, W_OK) == 0;
            case 'x': return access(path, X_OK) == 0;
            }
        }

        /* String tests */
        if (op == 'z') {
            next();
            char *s = next();
            if (!s) { error("missing operand"); return 0; }
            return strlen(s) == 0;
        }
        if (op == 'n') {
            next();
            char *s = next();
            if (!s) { error("missing operand"); return 0; }
            return strlen(s) != 0;
        }
    }

    /* Now consume the token as a string/integer operand */
    next();

    /* Check for binary operator */
    char *op = peek();
    if (!op) {
        /* Single string: true if non-empty */
        return strlen(tok) > 0;
    }

    /* String comparison */
    if (strcmp(op, "=") == 0) {
        next();
        char *rhs = next();
        if (!rhs) { error("missing operand"); return 0; }
        return strcmp(tok, rhs) == 0;
    }
    if (strcmp(op, "!=") == 0) {
        next();
        char *rhs = next();
        if (!rhs) { error("missing operand"); return 0; }
        return strcmp(tok, rhs) != 0;
    }

    /* Integer comparison */
    if (op[0] == '-' && op[1] != '\0') {
        if (strcmp(op, "-eq") == 0 || strcmp(op, "-ne") == 0 ||
            strcmp(op, "-lt") == 0 || strcmp(op, "-le") == 0 ||
            strcmp(op, "-gt") == 0 || strcmp(op, "-ge") == 0) {
            next();
            char *rhs = next();
            if (!rhs) { error("missing operand"); return 0; }
            long a = atol(tok);
            long b = atol(rhs);
            if (strcmp(op, "-eq") == 0) return a == b;
            if (strcmp(op, "-ne") == 0) return a != b;
            if (strcmp(op, "-lt") == 0) return a < b;
            if (strcmp(op, "-le") == 0) return a <= b;
            if (strcmp(op, "-gt") == 0) return a > b;
            if (strcmp(op, "-ge") == 0) return a >= b;
        }
    }

    /* Standalone non-empty string */
    return strlen(tok) > 0;
}

static int expr(void) {
    return primary();
}

int main(int ac, char *av[]) {
    /* Check if invoked as "[" */
    char *base = strrchr(av[0], '/');
    base = base ? base + 1 : av[0];
    if (strcmp(base, "[") == 0) {
        if (ac < 2 || strcmp(av[ac - 1], "]") != 0) {
            dprintf(2, "[: missing ']'\n");
            return 2;
        }
        ac--; /* hide the closing ] */
    }

    argc = ac;
    argv = av;
    pos = 1;

    if (pos >= argc)
        return 1; /* no expression => false */

    int result = expr();

    if (pos < argc) {
        error("extra operand");
        return 2;
    }

    return result ? 0 : 1;
}
