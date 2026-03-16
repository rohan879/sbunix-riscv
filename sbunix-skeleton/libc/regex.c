/*
 * regex.c — POSIX regex implementation for SBUnix libc.
 *
 * Recursive backtracking regex engine supporting both BRE and ERE.
 */

#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/*  Internal data structures                                          */
/* ------------------------------------------------------------------ */

enum re_opcode {
    RE_LITERAL,       /* match single character                       */
    RE_DOT,           /* match any character                          */
    RE_CLASS,         /* character class [abc]                        */
    RE_NCLASS,        /* negated class  [^abc]                        */
    RE_ANCHOR_BOL,    /* ^                                            */
    RE_ANCHOR_EOL,    /* $                                            */
    RE_GROUP_START,   /* ( or \(                                      */
    RE_GROUP_END,     /* ) or \)                                      */
    RE_ALT,           /* | alternation (ERE) or \| (BRE, extension)   */
    RE_BACKREF,       /* \1 .. \9                                     */
    RE_END            /* sentinel                                     */
};

struct re_node {
    enum re_opcode op;
    int ch;               /* literal char or group number             */
    int quant_min;         /* minimum repetitions                     */
    int quant_max;         /* maximum repetitions (-1 = unlimited)    */
    int greedy;            /* always 1 (POSIX requires greedy)        */
    unsigned char cclass[32]; /* 256-bit bitmap for char classes       */
    struct re_node *alt;   /* RE_ALT: right branch                    */
    struct re_node *alt_left; /* RE_ALT: left branch                  */
    struct re_node *next;  /* next node in sequence (continuation)    */
};

/* Internal compiled regex held behind regex_t.__priv */
struct re_prog {
    struct re_node *start;  /* first node in the pattern              */
    int  cflags;            /* REG_EXTENDED | REG_ICASE | ...         */
    int  nsub;              /* number of parenthesised groups         */
};

/* ------------------------------------------------------------------ */
/*  Helpers: character class bitmap                                   */
/* ------------------------------------------------------------------ */

static void cc_set(unsigned char *bm, int c)
{
    bm[(unsigned char)c >> 3] |= (unsigned char)(1 << ((unsigned char)c & 7));
}

static int cc_test(const unsigned char *bm, int c)
{
    return (bm[(unsigned char)c >> 3] >> ((unsigned char)c & 7)) & 1;
}

/* ------------------------------------------------------------------ */
/*  Node allocation                                                   */
/* ------------------------------------------------------------------ */

static struct re_node *node_new(enum re_opcode op)
{
    struct re_node *n = malloc(sizeof *n);
    if (!n) return NULL;
    memset(n, 0, sizeof *n);
    n->op = op;
    n->quant_min = 1;
    n->quant_max = 1;
    n->greedy = 1;
    return n;
}

static void node_free(struct re_node *n)
{
    while (n) {
        struct re_node *nx = n->next;
        if (n->alt) node_free(n->alt);
        if (n->alt_left) node_free(n->alt_left);
        free(n);
        n = nx;
    }
}

/* ------------------------------------------------------------------ */
/*  Parser state                                                      */
/* ------------------------------------------------------------------ */

struct parse_ctx {
    const char *pat;  /* current position in pattern */
    int cflags;
    int nsub;         /* group counter */
    int err;          /* first error code, 0 = ok */
};

static int is_ere(const struct parse_ctx *ctx)
{
    return ctx->cflags & REG_EXTENDED;
}

/* ------------------------------------------------------------------ */
/*  Forward declarations (parser)                                     */
/* ------------------------------------------------------------------ */

static struct re_node *parse_alt(struct parse_ctx *ctx);

/* ------------------------------------------------------------------ */
/*  Parser: character class [...]                                     */
/* ------------------------------------------------------------------ */

static void parse_posix_class(struct parse_ctx *ctx, unsigned char *bm)
{
    /* Handle POSIX character classes like [:alpha:] inside brackets */
    const char *p = ctx->pat;
    /* p points right after "[:" */
    const char *end = strstr(p, ":]");
    if (!end) { ctx->err = REG_ECTYPE; return; }

    size_t len = (size_t)(end - p);
    int i;

    if (len == 5 && strncmp(p, "alpha", 5) == 0) {
        for (i = 'A'; i <= 'Z'; i++) cc_set(bm, i);
        for (i = 'a'; i <= 'z'; i++) cc_set(bm, i);
    } else if (len == 5 && strncmp(p, "digit", 5) == 0) {
        for (i = '0'; i <= '9'; i++) cc_set(bm, i);
    } else if (len == 5 && strncmp(p, "alnum", 5) == 0) {
        for (i = 'A'; i <= 'Z'; i++) cc_set(bm, i);
        for (i = 'a'; i <= 'z'; i++) cc_set(bm, i);
        for (i = '0'; i <= '9'; i++) cc_set(bm, i);
    } else if (len == 5 && strncmp(p, "space", 5) == 0) {
        cc_set(bm, ' '); cc_set(bm, '\t'); cc_set(bm, '\n');
        cc_set(bm, '\r'); cc_set(bm, '\f'); cc_set(bm, '\v');
    } else if (len == 5 && strncmp(p, "upper", 5) == 0) {
        for (i = 'A'; i <= 'Z'; i++) cc_set(bm, i);
    } else if (len == 5 && strncmp(p, "lower", 5) == 0) {
        for (i = 'a'; i <= 'z'; i++) cc_set(bm, i);
    } else if (len == 5 && strncmp(p, "print", 5) == 0) {
        for (i = 0x20; i <= 0x7e; i++) cc_set(bm, i);
    } else if (len == 5 && strncmp(p, "graph", 5) == 0) {
        for (i = 0x21; i <= 0x7e; i++) cc_set(bm, i);
    } else if (len == 5 && strncmp(p, "punct", 5) == 0) {
        for (i = 0x21; i <= 0x7e; i++)
            if (!isalnum(i)) cc_set(bm, i);
    } else if (len == 5 && strncmp(p, "cntrl", 5) == 0) {
        for (i = 0; i < 0x20; i++) cc_set(bm, i);
        cc_set(bm, 0x7f);
    } else if (len == 6 && strncmp(p, "xdigit", 6) == 0) {
        for (i = '0'; i <= '9'; i++) cc_set(bm, i);
        for (i = 'A'; i <= 'F'; i++) cc_set(bm, i);
        for (i = 'a'; i <= 'f'; i++) cc_set(bm, i);
    } else if (len == 5 && strncmp(p, "blank", 5) == 0) {
        cc_set(bm, ' '); cc_set(bm, '\t');
    } else {
        ctx->err = REG_ECTYPE;
        return;
    }
    ctx->pat = end + 2; /* skip past ":]" */
}

static struct re_node *parse_bracket(struct parse_ctx *ctx)
{
    const char *p = ctx->pat; /* points past '[' */
    int negate = 0;

    if (*p == '^') { negate = 1; p++; }

    struct re_node *n = node_new(negate ? RE_NCLASS : RE_CLASS);
    if (!n) { ctx->err = REG_ESPACE; return NULL; }

    /* Handle leading ']' or '-' as literal */
    int first = 1;
    while (*p && (*p != ']' || first)) {
        first = 0;

        /* POSIX class [:foo:] */
        if (p[0] == '[' && p[1] == ':') {
            ctx->pat = p + 2;
            parse_posix_class(ctx, n->cclass);
            if (ctx->err) { node_free(n); return NULL; }
            p = ctx->pat;
            continue;
        }

        unsigned char lo = (unsigned char)*p++;
        if (*p == '-' && p[1] && p[1] != ']') {
            p++; /* skip '-' */
            unsigned char hi = (unsigned char)*p++;
            if (lo > hi) { ctx->err = REG_ERANGE; node_free(n); return NULL; }
            for (int c = lo; c <= hi; c++)
                cc_set(n->cclass, c);
        } else {
            cc_set(n->cclass, lo);
        }
    }

    if (*p != ']') { ctx->err = REG_EBRACK; node_free(n); return NULL; }
    p++; /* skip ']' */

    /* If REG_ICASE, duplicate each alpha char into the other case */
    if (ctx->cflags & REG_ICASE) {
        for (int c = 0; c < 256; c++) {
            if (cc_test(n->cclass, c)) {
                if (islower(c)) cc_set(n->cclass, toupper(c));
                else if (isupper(c)) cc_set(n->cclass, tolower(c));
            }
        }
    }

    ctx->pat = p;
    return n;
}

/* ------------------------------------------------------------------ */
/*  Parser: bounded repetition {m,n} or \{m,n\}                      */
/* ------------------------------------------------------------------ */

static int parse_bound(struct parse_ctx *ctx, int *pmin, int *pmax)
{
    const char *p = ctx->pat;
    int min_v = 0, max_v = -1;
    int has_min = 0, has_comma = 0;

    /* Parse minimum */
    while (*p >= '0' && *p <= '9') {
        min_v = min_v * 10 + (*p - '0');
        p++;
        has_min = 1;
    }
    if (!has_min) return REG_BADBR;

    if (*p == ',') {
        has_comma = 1;
        p++;
        if ((*p >= '0' && *p <= '9')) {
            max_v = 0;
            while (*p >= '0' && *p <= '9') {
                max_v = max_v * 10 + (*p - '0');
                p++;
            }
        } else {
            max_v = -1; /* unbounded */
        }
    } else {
        max_v = min_v; /* exact count */
    }

    /* Expect closing brace */
    if (is_ere(ctx)) {
        if (*p != '}') return REG_EBRACE;
        p++;
    } else {
        if (p[0] != '\\' || p[1] != '}') return REG_EBRACE;
        p += 2;
    }

    if (has_comma && max_v >= 0 && min_v > max_v)
        return REG_BADBR;

    *pmin = min_v;
    *pmax = max_v;
    ctx->pat = p;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Parser: single atom (literal, dot, class, group, anchor, backref) */
/* ------------------------------------------------------------------ */

static struct re_node *parse_atom(struct parse_ctx *ctx)
{
    const char *p = ctx->pat;
    struct re_node *n = NULL;

    if (*p == '\0') return NULL;

    /* ERE: '|' and ')' terminate an atom sequence */
    if (is_ere(ctx)) {
        if (*p == '|' || *p == ')') return NULL;
    }

    switch (*p) {
    /* Anchors */
    case '^':
        n = node_new(RE_ANCHOR_BOL);
        if (!n) { ctx->err = REG_ESPACE; return NULL; }
        n->quant_min = n->quant_max = 1;
        ctx->pat = p + 1;
        return n;

    case '$':
        n = node_new(RE_ANCHOR_EOL);
        if (!n) { ctx->err = REG_ESPACE; return NULL; }
        n->quant_min = n->quant_max = 1;
        ctx->pat = p + 1;
        return n;

    /* Dot */
    case '.':
        n = node_new(RE_DOT);
        if (!n) { ctx->err = REG_ESPACE; return NULL; }
        ctx->pat = p + 1;
        return n;

    /* Character class */
    case '[':
        ctx->pat = p + 1;
        return parse_bracket(ctx);

    /* ERE grouping */
    case '(':
        if (is_ere(ctx)) {
            ctx->pat = p + 1;
            int gnum = ++ctx->nsub;
            struct re_node *gs = node_new(RE_GROUP_START);
            if (!gs) { ctx->err = REG_ESPACE; return NULL; }
            gs->ch = gnum;
            gs->quant_min = gs->quant_max = 1;

            struct re_node *body = parse_alt(ctx);
            if (ctx->err) { node_free(gs); node_free(body); return NULL; }

            if (*ctx->pat != ')') {
                ctx->err = REG_EPAREN;
                node_free(gs); node_free(body);
                return NULL;
            }
            ctx->pat++; /* skip ')' */

            struct re_node *ge = node_new(RE_GROUP_END);
            if (!ge) { ctx->err = REG_ESPACE; node_free(gs); node_free(body); return NULL; }
            ge->ch = gnum;
            ge->quant_min = ge->quant_max = 1;

            /* Link: gs -> body -> ge */
            gs->next = body;
            struct re_node *tail = body;
            if (tail) {
                while (tail->next) tail = tail->next;
                tail->next = ge;
            } else {
                gs->next = ge;
            }
            return gs;
        }
        /* BRE: literal '(' */
        goto literal;

    /* ERE: these are special, but in BRE they're literal */
    case ')':
        if (is_ere(ctx)) return NULL; /* handled by caller */
        goto literal;

    case '{':
        if (is_ere(ctx)) goto literal; /* lone '{' is literal in ERE if not preceded by quantifiable atom */
        goto literal;

    /* ERE quantifiers handled elsewhere; BRE: literal */
    case '+':
    case '?':
        if (is_ere(ctx)) return NULL; /* should not reach here normally */
        goto literal;

    case '|':
        if (is_ere(ctx)) return NULL; /* alternation handled in parse_alt */
        goto literal;

    /* Backslash sequences */
    case '\\':
        p++;
        if (*p == '\0') { ctx->err = REG_EESCAPE; return NULL; }

        /* BRE grouping: \( ... \) */
        if (!is_ere(ctx) && *p == '(') {
            ctx->pat = p + 1;
            int gnum = ++ctx->nsub;
            struct re_node *gs = node_new(RE_GROUP_START);
            if (!gs) { ctx->err = REG_ESPACE; return NULL; }
            gs->ch = gnum;
            gs->quant_min = gs->quant_max = 1;

            struct re_node *body = parse_alt(ctx);
            if (ctx->err) { node_free(gs); node_free(body); return NULL; }

            if (ctx->pat[0] != '\\' || ctx->pat[1] != ')') {
                ctx->err = REG_EPAREN;
                node_free(gs); node_free(body);
                return NULL;
            }
            ctx->pat += 2; /* skip \) */

            struct re_node *ge = node_new(RE_GROUP_END);
            if (!ge) { ctx->err = REG_ESPACE; node_free(gs); node_free(body); return NULL; }
            ge->ch = gnum;
            ge->quant_min = ge->quant_max = 1;

            gs->next = body;
            struct re_node *tail = body;
            if (tail) {
                while (tail->next) tail = tail->next;
                tail->next = ge;
            } else {
                gs->next = ge;
            }
            return gs;
        }

        /* BRE: \) terminates a group — return NULL so caller sees it */
        if (!is_ere(ctx) && *p == ')') return NULL;

        /* Backreference \1..\9 */
        if (*p >= '1' && *p <= '9') {
            n = node_new(RE_BACKREF);
            if (!n) { ctx->err = REG_ESPACE; return NULL; }
            n->ch = *p - '0';
            ctx->pat = p + 1;
            return n;
        }

        /* BRE bounded repetition \{ ... \} */
        if (!is_ere(ctx) && *p == '{') {
            /* This shouldn't be hit as an atom — it's handled after an atom.
             * If we land here, it's an error or literal. Treat as literal '{'. */
            n = node_new(RE_LITERAL);
            if (!n) { ctx->err = REG_ESPACE; return NULL; }
            n->ch = '{';
            if (ctx->cflags & REG_ICASE)
                n->ch = tolower(n->ch);
            ctx->pat = p + 1;
            return n;
        }

        /* Escaped literal */
        n = node_new(RE_LITERAL);
        if (!n) { ctx->err = REG_ESPACE; return NULL; }
        n->ch = (unsigned char)*p;
        if (ctx->cflags & REG_ICASE) n->ch = tolower(n->ch);
        ctx->pat = p + 1;
        return n;

    /* ERE special chars that aren't quantifiers or grouping — literal in BRE */
    default:
literal:
        n = node_new(RE_LITERAL);
        if (!n) { ctx->err = REG_ESPACE; return NULL; }
        n->ch = (unsigned char)*p;
        if (ctx->cflags & REG_ICASE) n->ch = tolower(n->ch);
        ctx->pat = p + 1;
        return n;
    }
}

/* ------------------------------------------------------------------ */
/*  Parser: atom + optional quantifier                                */
/* ------------------------------------------------------------------ */

static struct re_node *parse_quantified(struct parse_ctx *ctx)
{
    struct re_node *n = parse_atom(ctx);
    if (!n || ctx->err) return n;

    const char *p = ctx->pat;

    /* Check for quantifier */
    if (is_ere(ctx)) {
        if (*p == '*') {
            n->quant_min = 0; n->quant_max = -1;
            ctx->pat = p + 1;
        } else if (*p == '+') {
            n->quant_min = 1; n->quant_max = -1;
            ctx->pat = p + 1;
        } else if (*p == '?') {
            n->quant_min = 0; n->quant_max = 1;
            ctx->pat = p + 1;
        } else if (*p == '{') {
            ctx->pat = p + 1;
            int qmin, qmax;
            int err = parse_bound(ctx, &qmin, &qmax);
            if (err) {
                /* In ERE, a lone '{' that doesn't start a valid bound is literal.
                 * But we already consumed it. Restore and treat quantifier as 1,1 (no-op). */
                ctx->pat = p; /* rewind — treat { as end of atom */
                return n;
            }
            n->quant_min = qmin;
            n->quant_max = qmax;
        }
    } else {
        /* BRE */
        if (*p == '*') {
            n->quant_min = 0; n->quant_max = -1;
            ctx->pat = p + 1;
        } else if (p[0] == '\\' && p[1] == '{') {
            ctx->pat = p + 2;
            int qmin, qmax;
            int err = parse_bound(ctx, &qmin, &qmax);
            if (err) { ctx->err = err; node_free(n); return NULL; }
            n->quant_min = qmin;
            n->quant_max = qmax;
        }
    }

    return n;
}

/* ------------------------------------------------------------------ */
/*  Parser: concatenation sequence                                    */
/* ------------------------------------------------------------------ */

static struct re_node *parse_concat(struct parse_ctx *ctx)
{
    struct re_node *head = NULL;
    struct re_node *tail = NULL;

    for (;;) {
        if (ctx->err) break;
        if (*ctx->pat == '\0') break;

        /* ERE alternation/group-end stop concatenation */
        if (is_ere(ctx) && (*ctx->pat == '|' || *ctx->pat == ')'))
            break;

        /* BRE: \| or \) stop concatenation */
        if (!is_ere(ctx) && ctx->pat[0] == '\\' &&
            (ctx->pat[1] == ')' || ctx->pat[1] == '|'))
            break;

        struct re_node *n = parse_quantified(ctx);
        if (!n) break;

        if (!head) {
            head = n;
        } else {
            tail->next = n;
        }
        /* advance tail to end of the returned chain (groups are chains) */
        tail = n;
        while (tail->next) tail = tail->next;
    }

    return head;
}

/* ------------------------------------------------------------------ */
/*  Parser: alternation                                               */
/* ------------------------------------------------------------------ */

static struct re_node *parse_alt(struct parse_ctx *ctx)
{
    struct re_node *left = parse_concat(ctx);
    if (ctx->err) return left;

    int have_alt = 0;
    if (is_ere(ctx)) {
        have_alt = (*ctx->pat == '|');
    } else {
        have_alt = (ctx->pat[0] == '\\' && ctx->pat[1] == '|');
    }

    if (!have_alt) return left;

    /* Build alternation node.
     * alt_left  = left branch (self-contained chain, ends with NULL next)
     * alt       = right branch (may itself be RE_ALT for chained |)
     * next      = continuation (set later by caller via parse_concat linking)
     */
    struct re_node *alt_node = node_new(RE_ALT);
    if (!alt_node) { ctx->err = REG_ESPACE; node_free(left); return NULL; }
    alt_node->quant_min = alt_node->quant_max = 1;
    alt_node->alt_left = left;   /* first alternative */

    /* skip '|' or '\|' */
    if (is_ere(ctx)) ctx->pat++;
    else ctx->pat += 2;

    struct re_node *right = parse_alt(ctx); /* recursive for right-assoc chaining */
    if (ctx->err) { node_free(alt_node); node_free(right); return NULL; }

    alt_node->alt = right;  /* second alternative */
    /* alt_node->next remains NULL; set by caller (parse_concat or regcomp) */
    return alt_node;
}

/* ------------------------------------------------------------------ */
/*  regcomp()                                                         */
/* ------------------------------------------------------------------ */

int regcomp(regex_t *preg, const char *pattern, int cflags)
{
    struct re_prog *prog = malloc(sizeof *prog);
    if (!prog) return REG_ESPACE;
    memset(prog, 0, sizeof *prog);
    prog->cflags = cflags;

    struct parse_ctx ctx;
    ctx.pat = pattern;
    ctx.cflags = cflags;
    ctx.nsub = 0;
    ctx.err = 0;

    struct re_node *tree = parse_alt(&ctx);

    if (ctx.err) {
        node_free(tree);
        free(prog);
        return ctx.err;
    }

    /* Should have consumed entire pattern */
    if (*ctx.pat != '\0') {
        /* Unbalanced parens or leftover chars */
        node_free(tree);
        free(prog);
        return REG_BADPAT;
    }

    /* Append sentinel */
    struct re_node *end = node_new(RE_END);
    if (!end) { node_free(tree); free(prog); return REG_ESPACE; }

    if (tree) {
        struct re_node *t = tree;
        while (t->next) t = t->next;
        t->next = end;
    } else {
        tree = end;
    }

    prog->start = tree;
    prog->nsub = ctx.nsub;

    preg->re_nsub = (size_t)ctx.nsub;
    preg->__priv = prog;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Matcher state                                                     */
/* ------------------------------------------------------------------ */

#define RE_MAX_GROUPS 10  /* group 0 = whole match, 1..9 = subs */
#define RE_MAX_RECURSION 4096

struct match_ctx {
    const char *str;         /* start of input string */
    const char *str_end;     /* pointer past last char */
    int cflags;
    int eflags;
    /* Group capture bookkeeping */
    const char *group_start[RE_MAX_GROUPS];
    const char *group_end[RE_MAX_GROUPS];
    int depth;               /* recursion depth guard */
};

/* ------------------------------------------------------------------ */
/*  Matcher: does character c match at the given node?                */
/* ------------------------------------------------------------------ */

static int char_match(const struct match_ctx *mx, const struct re_node *n, int c)
{
    int ic = (mx->cflags & REG_ICASE) ? tolower(c) : c;

    switch (n->op) {
    case RE_LITERAL:
        return ic == n->ch;

    case RE_DOT:
        if (c == '\n' && (mx->cflags & REG_NEWLINE))
            return 0;
        return 1;

    case RE_CLASS:
        return cc_test(n->cclass, (unsigned char)c);

    case RE_NCLASS:
        if (c == '\n' && (mx->cflags & REG_NEWLINE))
            return 0;
        return !cc_test(n->cclass, (unsigned char)c);

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Matcher core: recursive backtracking                              */
/* ------------------------------------------------------------------ */

/*
 * match_here() tries to match 'node' and everything after it against
 * the string starting at 'sp'.  Returns a pointer past the matched
 * portion on success, or NULL on failure.
 */
static const char *match_here(struct match_ctx *mx,
                              const struct re_node *node,
                              const char *sp);

/* Try matching a quantified node (the node's quant_min / quant_max). */
static const char *match_quant(struct match_ctx *mx,
                               const struct re_node *node,
                               const char *sp)
{
    int qmin = node->quant_min;
    int qmax = node->quant_max;
    const struct re_node *rest = node->next;

    if (mx->depth > RE_MAX_RECURSION) return NULL;

    /*
     * Greedy matching: try as many as possible, then backtrack.
     * Collect all possible match positions.
     */

    /* Count how many times we can match the atom at sp. */
    int count = 0;

    /* We need to store positions for backtracking.
     * Use a stack-allocated array with a reasonable limit.
     */
    #define MAX_QUANT_TRACK 1024
    const char *positions[MAX_QUANT_TRACK + 1];
    positions[0] = sp;

    while ((qmax < 0 || count < qmax) && count < MAX_QUANT_TRACK) {
        const char *cur = positions[count];
        if (cur >= mx->str_end && node->op != RE_ANCHOR_EOL && node->op != RE_ANCHOR_BOL)
            break;
        if (cur < mx->str || cur > mx->str_end)
            break;

        /* For backreference, try one match of the referenced group text */
        if (node->op == RE_BACKREF) {
            int gn = node->ch;
            if (gn >= RE_MAX_GROUPS || !mx->group_start[gn] || !mx->group_end[gn])
                break;
            const char *gs = mx->group_start[gn];
            const char *ge = mx->group_end[gn];
            size_t glen = (size_t)(ge - gs);
            if ((size_t)(mx->str_end - cur) < glen) break;
            int ok = 1;
            for (size_t i = 0; i < glen; i++) {
                int a = (unsigned char)cur[i];
                int b = (unsigned char)gs[i];
                if (mx->cflags & REG_ICASE) { a = tolower(a); b = tolower(b); }
                if (a != b) { ok = 0; break; }
            }
            if (!ok) break;
            count++;
            positions[count] = cur + glen;
            continue;
        }

        /* For ordinary char-matching opcodes */
        if (node->op == RE_LITERAL || node->op == RE_DOT ||
            node->op == RE_CLASS || node->op == RE_NCLASS) {
            if (cur >= mx->str_end) break;
            if (!char_match(mx, node, (unsigned char)*cur))
                break;
            count++;
            positions[count] = cur + 1;
            continue;
        }

        /* Non-char opcodes (anchors, groups, etc.) should not be quantified
         * in a way that reaches here, but handle gracefully. */
        break;
    }

    /* Now backtrack from max matches to min */
    for (int i = count; i >= qmin; i--) {
        const char *res = match_here(mx, rest, positions[i]);
        if (res) return res;
    }

    return NULL;
}

static const char *match_here(struct match_ctx *mx,
                              const struct re_node *node,
                              const char *sp)
{
    if (!node) return sp;  /* no more pattern = success */
    if (mx->depth++ > RE_MAX_RECURSION) { mx->depth--; return NULL; }

    const char *result = NULL;

    switch (node->op) {

    case RE_END:
        result = sp;
        break;

    case RE_ANCHOR_BOL:
        if (sp == mx->str) {
            if (!(mx->eflags & REG_NOTBOL))
                result = match_here(mx, node->next, sp);
        } else if ((mx->cflags & REG_NEWLINE) && sp > mx->str && sp[-1] == '\n') {
            result = match_here(mx, node->next, sp);
        }
        break;

    case RE_ANCHOR_EOL:
        if (*sp == '\0') {
            if (!(mx->eflags & REG_NOTEOL))
                result = match_here(mx, node->next, sp);
        } else if ((mx->cflags & REG_NEWLINE) && *sp == '\n') {
            result = match_here(mx, node->next, sp);
        }
        break;

    case RE_GROUP_START: {
        int gn = node->ch;
        int qmin = node->quant_min;
        int qmax = node->quant_max;

        if (qmin == 1 && qmax == 1) {
            /* Simple case: group matched exactly once */
            const char *old_start = NULL;
            const char *old_end = NULL;
            if (gn < RE_MAX_GROUPS) {
                old_start = mx->group_start[gn];
                old_end = mx->group_end[gn];
                mx->group_start[gn] = sp;
                mx->group_end[gn] = NULL;
            }
            result = match_here(mx, node->next, sp);
            if (!result && gn < RE_MAX_GROUPS) {
                mx->group_start[gn] = old_start;
                mx->group_end[gn] = old_end;
            }
        } else {
            /* Quantified group: find the matching GROUP_END to get body + continuation */
            struct re_node *ge = (struct re_node *)node->next;
            int depth_count = 1;
            while (ge) {
                if (ge->op == RE_GROUP_START) depth_count++;
                else if (ge->op == RE_GROUP_END) {
                    depth_count--;
                    if (depth_count == 0) break;
                }
                ge = ge->next;
            }
            if (!ge) break; /* malformed: no matching GROUP_END */

            const struct re_node *continuation = ge->next;

            /* Temporarily disconnect GROUP_END from continuation so that
             * match_here on the body stops after GROUP_END. */
            struct re_node *saved_ge_next = ge->next;
            ge->next = NULL;

            /* Save old group state */
            const char *old_start = NULL;
            const char *old_end = NULL;
            if (gn < RE_MAX_GROUPS) {
                old_start = mx->group_start[gn];
                old_end = mx->group_end[gn];
            }

            /* Greedy: try max matches, backtrack to min.
             * Collect positions after each full group match. */
            const char *gpositions[MAX_QUANT_TRACK + 1];
            gpositions[0] = sp;
            int gcount = 0;

            while ((qmax < 0 || gcount < qmax) && gcount < MAX_QUANT_TRACK) {
                const char *cur = gpositions[gcount];
                if (cur > mx->str_end) break;

                /* Try matching the group body once from cur */
                if (gn < RE_MAX_GROUPS) {
                    mx->group_start[gn] = cur;
                    mx->group_end[gn] = NULL;
                }
                /* Match body: from node->next through ge (ge->next is NULL) */
                const char *body_end = match_here(mx, node->next, cur);
                if (!body_end) break;
                /* Avoid infinite loop on zero-length match */
                if (body_end == cur && gcount > 0) break;
                gcount++;
                gpositions[gcount] = body_end;
            }

            /* Restore GROUP_END linkage */
            ge->next = saved_ge_next;

            /* Backtrack from max to min */
            result = NULL;
            for (int gi = gcount; gi >= qmin; gi--) {
                /* Set group capture to last iteration (or clear if 0 iterations) */
                if (gn < RE_MAX_GROUPS) {
                    if (gi > 0) {
                        mx->group_start[gn] = gpositions[gi - 1];
                        mx->group_end[gn] = gpositions[gi];
                    } else {
                        mx->group_start[gn] = NULL;
                        mx->group_end[gn] = NULL;
                    }
                }
                result = match_here(mx, continuation, gpositions[gi]);
                if (result) break;
            }

            /* Restore on failure */
            if (!result && gn < RE_MAX_GROUPS) {
                mx->group_start[gn] = old_start;
                mx->group_end[gn] = old_end;
            }
        }
        break;
    }

    case RE_GROUP_END: {
        int gn = node->ch;
        const char *old_end = NULL;
        if (gn < RE_MAX_GROUPS) {
            old_end = mx->group_end[gn];
            mx->group_end[gn] = sp;
        }
        result = match_here(mx, node->next, sp);
        if (!result && gn < RE_MAX_GROUPS) {
            mx->group_end[gn] = old_end;
        }
        break;
    }

    case RE_ALT: {
        /* alt_left = left branch, alt = right branch, next = continuation */
        /* Try left branch, then continue with node->next */
        const char *branch_end = match_here(mx, node->alt_left, sp);
        if (branch_end) {
            result = match_here(mx, node->next, branch_end);
        }
        /* If left didn't work, try right branch, then continuation */
        if (!result) {
            branch_end = match_here(mx, node->alt, sp);
            if (branch_end) {
                result = match_here(mx, node->next, branch_end);
            }
        }
        break;
    }

    case RE_LITERAL:
    case RE_DOT:
    case RE_CLASS:
    case RE_NCLASS:
    case RE_BACKREF:
        /* These opcodes may have quantifiers */
        if (node->quant_min == 1 && node->quant_max == 1) {
            /* Simple case: exactly once */
            if (node->op == RE_BACKREF) {
                int gn = node->ch;
                if (gn >= RE_MAX_GROUPS || !mx->group_start[gn] || !mx->group_end[gn])
                    break;
                const char *gs = mx->group_start[gn];
                const char *ge = mx->group_end[gn];
                size_t glen = (size_t)(ge - gs);
                if ((size_t)(mx->str_end - sp) < glen) break;
                for (size_t i = 0; i < glen; i++) {
                    int a = (unsigned char)sp[i];
                    int b = (unsigned char)gs[i];
                    if (mx->cflags & REG_ICASE) { a = tolower(a); b = tolower(b); }
                    if (a != b) goto done;
                }
                result = match_here(mx, node->next, sp + glen);
            } else {
                if (sp >= mx->str_end) break;
                if (!char_match(mx, node, (unsigned char)*sp)) break;
                result = match_here(mx, node->next, sp + 1);
            }
        } else {
            result = match_quant(mx, node, sp);
        }
        break;

    default:
        break;
    }

done:
    mx->depth--;
    return result;
}

/* ------------------------------------------------------------------ */
/*  regexec()                                                         */
/* ------------------------------------------------------------------ */

int regexec(const regex_t *preg, const char *string,
            size_t nmatch, regmatch_t pmatch[], int eflags)
{
    struct re_prog *prog = preg->__priv;
    if (!prog || !prog->start) return REG_NOMATCH;

    struct match_ctx mx;
    memset(&mx, 0, sizeof mx);
    mx.str = string;
    mx.str_end = string + strlen(string);
    mx.cflags = prog->cflags;
    mx.eflags = eflags;

    int nosub = (prog->cflags & REG_NOSUB) || nmatch == 0 || !pmatch;

    /*
     * Try matching at each position in the string.
     * For a pattern anchored with ^, we only need to try position 0
     * (and after each newline if REG_NEWLINE is set).
     */
    int anchored = (prog->start->op == RE_ANCHOR_BOL);

    for (const char *sp = string; sp <= mx.str_end; sp++) {
        /* Reset group captures */
        for (int g = 0; g < RE_MAX_GROUPS; g++) {
            mx.group_start[g] = NULL;
            mx.group_end[g] = NULL;
        }
        mx.depth = 0;
        mx.group_start[0] = sp;

        const char *end = match_here(&mx, prog->start, sp);
        if (end) {
            mx.group_end[0] = end;

            /* Populate pmatch */
            if (!nosub) {
                for (size_t i = 0; i < nmatch; i++) {
                    if (i == 0) {
                        pmatch[0].rm_so = (int)(sp - string);
                        pmatch[0].rm_eo = (int)(end - string);
                    } else if ((int)i <= prog->nsub &&
                               mx.group_start[i] && mx.group_end[i]) {
                        pmatch[i].rm_so = (int)(mx.group_start[i] - string);
                        pmatch[i].rm_eo = (int)(mx.group_end[i] - string);
                    } else {
                        pmatch[i].rm_so = -1;
                        pmatch[i].rm_eo = -1;
                    }
                }
            }
            return 0;
        }

        /* If anchored and REG_NEWLINE not set, no point trying further */
        if (anchored && !(prog->cflags & REG_NEWLINE))
            break;

        /* If anchored with REG_NEWLINE, only try after newline */
        if (anchored && (prog->cflags & REG_NEWLINE)) {
            if (*sp != '\n') {
                /* advance to next newline */
                const char *nl = strchr(sp, '\n');
                if (!nl) break;
                sp = nl; /* loop will increment past it */
            }
        }

        /* Don't advance past end of string */
        if (*sp == '\0') break;
    }

    return REG_NOMATCH;
}

/* ------------------------------------------------------------------ */
/*  regerror()                                                        */
/* ------------------------------------------------------------------ */

static const char *re_errmsgs[] = {
    "Success",                           /* 0            */
    "No match",                          /* REG_NOMATCH  */
    "Invalid regular expression",        /* REG_BADPAT   */
    "Invalid collating element",         /* REG_ECOLLATE */
    "Invalid character class name",      /* REG_ECTYPE   */
    "Trailing backslash",                /* REG_EESCAPE  */
    "Invalid back reference",            /* REG_ESUBREG  */
    "Unmatched [ or [^",                 /* REG_EBRACK   */
    "Unmatched ( or \\(",                /* REG_EPAREN   */
    "Unmatched \\{",                     /* REG_EBRACE   */
    "Invalid content of \\{\\}",         /* REG_BADBR    */
    "Invalid range end",                 /* REG_ERANGE   */
    "Out of memory",                     /* REG_ESPACE   */
    "Invalid preceding regular expression" /* REG_BADRPT */
};

#define RE_NERR ((int)(sizeof re_errmsgs / sizeof re_errmsgs[0]))

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size)
{
    (void)preg;
    const char *msg;

    if (errcode >= 0 && errcode < RE_NERR)
        msg = re_errmsgs[errcode];
    else
        msg = "Unknown error";

    size_t len = strlen(msg) + 1; /* include NUL */

    if (errbuf && errbuf_size > 0) {
        size_t copy = (len < errbuf_size) ? len : errbuf_size;
        memcpy(errbuf, msg, copy - 1);
        errbuf[copy - 1] = '\0';
    }

    return len;
}

/* ------------------------------------------------------------------ */
/*  regfree()                                                         */
/* ------------------------------------------------------------------ */

void regfree(regex_t *preg)
{
    if (!preg) return;
    struct re_prog *prog = preg->__priv;
    if (prog) {
        node_free(prog->start);
        free(prog);
    }
    preg->__priv = NULL;
    preg->re_nsub = 0;
}
