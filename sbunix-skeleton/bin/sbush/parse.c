#include "sbush.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>

static char expand_buf[MAX_LINE];

/* Helper: emit string to dst, return updated dst */
static char *emit_str(char *dst, char *end, const char *s) {
    while (*s && dst < end)
        *dst++ = *s++;
    return dst;
}

/* -----------------------------------------------------------------------
 * Command substitution: $(cmd) and `cmd`
 * Fork a child, capture stdout via pipe, insert output.
 * ---------------------------------------------------------------------- */
static char *run_cmdsub(const char *cmd, char *dst, char *end) {
    int pfd[2];
    if (pipe(pfd) < 0)
        return dst;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return dst;
    }
    if (pid == 0) {
        /* Child: redirect stdout to pipe, run command */
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        /* Reset function call depth and exec depth (child is independent) */
        func_reset_depth();
        exec_reset_depth();
        /* Restore default signal handlers */
        struct sigaction sa;
        sa.sa_handler = SIG_DFL;
        sa.sa_mask = 0;
        sa.sa_flags = 0;
        sa.sa_restorer = NULL;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTSTP, &sa, NULL);
        sigaction(SIGCHLD, &sa, NULL);
        /* Execute via the shell itself */
        static char cmdbuf[256];
        strncpy(cmdbuf, cmd, 255);
        cmdbuf[255] = '\0';
        execute_line(cmdbuf);
        _exit(last_exit_status);
    }

    /* Parent: read child's stdout */
    close(pfd[1]);
    char ch;
    while (dst < end && read(pfd[0], &ch, 1) == 1) {
        *dst++ = ch;
    }
    close(pfd[0]);
    waitpid(pid, NULL, 0);

    /* Strip trailing newlines */
    while (dst > expand_buf && *(dst - 1) == '\n')
        dst--;

    return dst;
}

/* $(cmd): src points past '('. Find matching ')' and run. */
static char *expand_cmdsub(char **srcp, char *dst, char *end) {
    char *src = *srcp;
    static char cs_buf[512];
    int ci = 0, depth = 1;
    while (*src && depth > 0 && ci < 511) {
        if (*src == '(') depth++;
        else if (*src == ')') { depth--; if (depth == 0) { src++; break; } }
        cs_buf[ci++] = *src++;
    }
    cs_buf[ci] = '\0';
    *srcp = src;
    return run_cmdsub(cs_buf, dst, end);
}

/* -----------------------------------------------------------------------
 * Arithmetic expansion: $((...))
 * Recursive descent parser: expr → cmp ( (==|!=|<|>|<=|>=) cmp )*
 *   cmp → term ( (+|-) term )*
 *   term → factor ( (*|/|%) factor )*
 *   factor → unary | '(' expr ')' | number | $VAR | VAR
 *   unary → -factor | +factor | !factor
 * ---------------------------------------------------------------------- */
static const char *arith_p;

static long arith_expr(void);

static void arith_skip_ws(void) {
    while (*arith_p == ' ' || *arith_p == '\t')
        arith_p++;
}

static long arith_factor(void) {
    arith_skip_ws();
    if (*arith_p == '(') {
        arith_p++;
        long v = arith_expr();
        arith_skip_ws();
        if (*arith_p == ')') arith_p++;
        return v;
    }
    if (*arith_p == '-') { arith_p++; return -arith_factor(); }
    if (*arith_p == '+') { arith_p++; return arith_factor(); }
    if (*arith_p == '!') { arith_p++; return !arith_factor(); }
    if (*arith_p == '$') {
        arith_p++;
        if (*arith_p == '?') { arith_p++; return (long)last_exit_status; }
        if (*arith_p == '#') { arith_p++; return (long)positional_count; }
        if (*arith_p >= '0' && *arith_p <= '9') {
            int idx = *arith_p - '0'; arith_p++;
            char *val = positional_get(idx);
            return val ? atol(val) : 0;
        }
        char name[64];
        int i = 0;
        while (is_name_char(*arith_p) && i < 63)
            name[i++] = *arith_p++;
        name[i] = '\0';
        char *val = var_get(name);
        return val ? atol(val) : 0;
    }
    if (*arith_p >= '0' && *arith_p <= '9') {
        long v = 0;
        while (*arith_p >= '0' && *arith_p <= '9')
            v = v * 10 + (*arith_p++ - '0');
        return v;
    }
    /* bare variable name */
    if (is_name_char(*arith_p)) {
        char name[64];
        int i = 0;
        while (is_name_char(*arith_p) && i < 63)
            name[i++] = *arith_p++;
        name[i] = '\0';
        char *val = var_get(name);
        return val ? atol(val) : 0;
    }
    return 0;
}

static long arith_term(void) {
    long v = arith_factor();
    while (1) {
        arith_skip_ws();
        if (*arith_p == '*') { arith_p++; v *= arith_factor(); }
        else if (*arith_p == '/' && arith_p[1] != '/') {
            arith_p++;
            long d = arith_factor();
            v = d ? v / d : 0;
        }
        else if (*arith_p == '%') { arith_p++; long d = arith_factor(); v = d ? v % d : 0; }
        else break;
    }
    return v;
}

static long arith_add(void) {
    long v = arith_term();
    while (1) {
        arith_skip_ws();
        if (*arith_p == '+') { arith_p++; v += arith_term(); }
        else if (*arith_p == '-') { arith_p++; v -= arith_term(); }
        else break;
    }
    return v;
}

static long arith_expr(void) {
    long v = arith_add();
    while (1) {
        arith_skip_ws();
        if (arith_p[0] == '=' && arith_p[1] == '=') { arith_p += 2; v = (v == arith_add()); }
        else if (arith_p[0] == '!' && arith_p[1] == '=') { arith_p += 2; v = (v != arith_add()); }
        else if (arith_p[0] == '<' && arith_p[1] == '=') { arith_p += 2; v = (v <= arith_add()); }
        else if (arith_p[0] == '>' && arith_p[1] == '=') { arith_p += 2; v = (v >= arith_add()); }
        else if (*arith_p == '<') { arith_p++; v = (v < arith_add()); }
        else if (*arith_p == '>') { arith_p++; v = (v > arith_add()); }
        else break;
    }
    return v;
}

/* Expand $((...)); src points past '((' */
static char *expand_arith(char **srcp, char *dst, char *end) {
    char *src = *srcp;
    /* Find matching )) */
    static char arith_buf[256];
    int ai = 0, depth = 2;
    while (*src && ai < 255) {
        if (src[0] == ')' && src[1] == ')') {
            depth -= 2;
            if (depth <= 0) { src += 2; break; }
        }
        if (src[0] == '(' && src[1] == '(') depth += 2;
        arith_buf[ai++] = *src++;
    }
    arith_buf[ai] = '\0';
    *srcp = src;

    arith_p = arith_buf;
    long result = arith_expr();
    char tmp[24];
    /* Convert long to string */
    int neg = 0;
    long r = result;
    if (r < 0) { neg = 1; r = -r; }
    int ti = 0;
    if (r == 0) tmp[ti++] = '0';
    while (r > 0) { tmp[ti++] = '0' + (int)(r % 10); r /= 10; }
    if (neg) tmp[ti++] = '-';
    /* reverse */
    for (int j = 0; j < ti / 2; j++) {
        char c = tmp[j]; tmp[j] = tmp[ti - 1 - j]; tmp[ti - 1 - j] = c;
    }
    tmp[ti] = '\0';
    return emit_str(dst, end, tmp);
}

/* Handle ${...} parameter expansion; src points past '{'. Returns updated dst. */
static char *expand_braces(char **srcp, char *dst, char *end) {
    char *src = *srcp;

    /* ${#VAR} — string length */
    if (*src == '#' && src[1] != '}') {
        src++;
        char name[64];
        int i = 0;
        while (*src && *src != '}' && i < 63)
            name[i++] = *src++;
        name[i] = '\0';
        if (*src == '}') src++;
        *srcp = src;
        char *val = var_get(name);
        char tmp[16];
        itoa(val ? (int)strlen(val) : 0, tmp, 10);
        return emit_str(dst, end, tmp);
    }

    /* Extract variable name */
    char name[64];
    int ni = 0;
    while (*src && is_name_char(*src) && ni < 63)
        name[ni++] = *src++;
    name[ni] = '\0';

    char *val = var_get(name);

    if (*src == '}') {
        /* ${VAR} — simple expansion */
        src++;
        *srcp = src;
        if (val)
            dst = emit_str(dst, end, val);
        return dst;
    }

    /* Determine operator */
    char op1 = *src++;
    char op2 = '\0';
    if ((op1 == '#' && *src == '#') || (op1 == '%' && *src == '%'))
        op2 = *src++;
    else if (op1 == ':')
        op2 = *src++;

    /* Extract operand until matching } */
    char operand[256];
    int oi = 0;
    int brace_depth = 1;
    while (*src && brace_depth > 0 && oi < 255) {
        if (*src == '{') brace_depth++;
        else if (*src == '}') { brace_depth--; if (brace_depth == 0) break; }
        operand[oi++] = *src++;
    }
    operand[oi] = '\0';
    if (*src == '}') src++;
    *srcp = src;

    if (op1 == ':') {
        int is_empty = (!val || val[0] == '\0');
        if (op2 == '-') {
            /* ${VAR:-default} */
            return emit_str(dst, end, is_empty ? operand : val);
        } else if (op2 == '=') {
            /* ${VAR:=default} */
            if (is_empty) {
                var_set(name, operand);
                val = operand;
            }
            return emit_str(dst, end, val ? val : operand);
        } else if (op2 == '+') {
            /* ${VAR:+alternate} */
            if (!is_empty)
                dst = emit_str(dst, end, operand);
            return dst;
        } else if (op2 == '?') {
            /* ${VAR:?message} */
            if (is_empty)
                dprintf(2, "sbush: %s: %s\n", name,
                        oi ? operand : "parameter null or not set");
            else
                dst = emit_str(dst, end, val);
            return dst;
        }
    }

    if (op1 == '#' && val) {
        /* ${VAR#pattern} or ${VAR##pattern} — prefix removal */
        static char pe_vbuf[MAX_LINE];
        strncpy(pe_vbuf, val, MAX_LINE - 1);
        pe_vbuf[MAX_LINE - 1] = '\0';
        int vlen = strlen(pe_vbuf);
        char *result = pe_vbuf;
        if (op2 == '#') {
            /* longest prefix */
            for (int k = vlen; k >= 0; k--) {
                char saved = pe_vbuf[k];
                pe_vbuf[k] = '\0';
                if (glob_match(operand, pe_vbuf)) {
                    pe_vbuf[k] = saved;
                    result = pe_vbuf + k;
                    break;
                }
                pe_vbuf[k] = saved;
            }
        } else {
            /* shortest prefix */
            for (int k = 0; k <= vlen; k++) {
                char saved = pe_vbuf[k];
                pe_vbuf[k] = '\0';
                if (glob_match(operand, pe_vbuf)) {
                    pe_vbuf[k] = saved;
                    result = pe_vbuf + k;
                    break;
                }
                pe_vbuf[k] = saved;
            }
        }
        return emit_str(dst, end, result);
    }

    if (op1 == '%' && val) {
        /* ${VAR%pattern} or ${VAR%%pattern} — suffix removal */
        static char pe_vbuf2[MAX_LINE];
        strncpy(pe_vbuf2, val, MAX_LINE - 1);
        pe_vbuf2[MAX_LINE - 1] = '\0';
        int vlen = strlen(pe_vbuf2);
        int cut = vlen;
        if (op2 == '%') {
            /* longest suffix */
            for (int k = 0; k <= vlen; k++) {
                if (glob_match(operand, pe_vbuf2 + k)) {
                    cut = k;
                    break;
                }
            }
        } else {
            /* shortest suffix */
            for (int k = vlen; k >= 0; k--) {
                if (glob_match(operand, pe_vbuf2 + k)) {
                    cut = k;
                    break;
                }
            }
        }
        for (int k = 0; k < cut && dst < end; k++)
            *dst++ = pe_vbuf2[k];
        return dst;
    }

    /* Unrecognized — just emit the value */
    if (val)
        dst = emit_str(dst, end, val);
    return dst;
}

static void expand_vars(char *line) {
    char *src = line;
    char *dst = expand_buf;
    char *end = expand_buf + MAX_LINE - 1;
    int in_sq = 0;

    while (*src && dst < end) {
        if (*src == '\'' ) {
            in_sq = !in_sq;
            *dst++ = *src++;
        } else if (*src == '$' && !in_sq) {
            src++;
            if (*src == '?') {
                /* $? -- last exit status */
                src++;
                char tmp[16];
                itoa(last_exit_status, tmp, 10);
                for (char *v = tmp; *v && dst < end; )
                    *dst++ = *v++;
            } else if (*src == '!') {
                /* $! -- last background PID */
                src++;
                char tmp[16];
                itoa((int)last_bg_pid, tmp, 10);
                for (char *v = tmp; *v && dst < end; )
                    *dst++ = *v++;
            } else if (*src == '$') {
                /* $$ -- shell PID */
                src++;
                char tmp[16];
                itoa((int)getpid(), tmp, 10);
                for (char *v = tmp; *v && dst < end; )
                    *dst++ = *v++;
            } else if (*src == '#') {
                /* $# -- positional parameter count */
                src++;
                char tmp[16];
                itoa(positional_count, tmp, 10);
                for (char *v = tmp; *v && dst < end; )
                    *dst++ = *v++;
            } else if (*src == '@' || *src == '*') {
                /* $@ / $* -- all positional parameters */
                src++;
                for (int pi = 1; pi <= positional_count; pi++) {
                    char *pv = positional_get(pi);
                    if (pv) {
                        if (pi > 1 && dst < end) *dst++ = ' ';
                        while (*pv && dst < end) *dst++ = *pv++;
                    }
                }
            } else if (*src >= '0' && *src <= '9') {
                /* $0-$9 -- positional parameter */
                int idx = *src - '0';
                src++;
                char *pv = positional_get(idx);
                if (pv) {
                    while (*pv && dst < end) *dst++ = *pv++;
                }
            } else if (*src == '(' && *(src+1) == '(') {
                /* $(( ... )) — arithmetic expansion */
                src += 2; /* skip (( */
                dst = expand_arith(&src, dst, end);
            } else if (*src == '(') {
                /* $(cmd) — command substitution */
                src++; /* skip ( */
                dst = expand_cmdsub(&src, dst, end);
            } else if (*src == '{') {
                src++; /* skip { */
                dst = expand_braces(&src, dst, end);
            } else {
                /* $NAME -- regular variable */
                char name[64];
                int i = 0;
                while (*src && ((*src >= 'a' && *src <= 'z') ||
                                (*src >= 'A' && *src <= 'Z') ||
                                (*src >= '0' && *src <= '9') ||
                                *src == '_') && i < 63) {
                    name[i++] = *src++;
                }
                name[i] = '\0';
                char *val = var_get(name);
                if (val) {
                    while (*val && dst < end)
                        *dst++ = *val++;
                }
            }
        } else if (*src == '`' && !in_sq) {
            /* `cmd` — backtick command substitution */
            src++; /* skip opening backtick */
            /* Find closing backtick and extract command */
            static char bt_cmd[256];
            int bi = 0;
            while (*src && *src != '`' && bi < 255)
                bt_cmd[bi++] = *src++;
            bt_cmd[bi] = '\0';
            if (*src == '`') src++;
            dst = run_cmdsub(bt_cmd, dst, end);
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    strcpy(line, expand_buf);
}

static int tokenize(char *str, struct command *cmd) {
    cmd->argc = 0;
    char *p = str;

    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        if (cmd->argc >= MAX_ARGS - 1)
            break;

        if (*p == '"') {
            p++;
            char *start = p;
            char *w = p;  /* in-place de-escape */
            while (*p && *p != '"') {
                if (*p == '\\' && *(p + 1)) {
                    p++;
                    switch (*p) {
                        case 'n':  *w++ = '\n'; break;
                        case 't':  *w++ = '\t'; break;
                        case '\\': *w++ = '\\'; break;
                        case '"':  *w++ = '"';  break;
                        case '$':  *w++ = '$';  break;
                        default:   *w++ = '\\'; *w++ = *p; break;
                    }
                    p++;
                } else {
                    *w++ = *p++;
                }
            }
            *w = '\0';
            if (*p == '"') p++;
            cmd->argv[cmd->argc++] = start;
        } else if (*p == '\'') {
            p++;
            cmd->argv[cmd->argc++] = p;
            while (*p && *p != '\'')
                p++;
            if (*p == '\'')
                *p++ = '\0';
        } else {
            cmd->argv[cmd->argc++] = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
            if (*p)
                *p++ = '\0';
        }
    }
    cmd->argv[cmd->argc] = NULL;

    /* Extract redirections from argv */
    cmd->redir_in = NULL;
    cmd->redir_out = NULL;
    cmd->redir_append = NULL;
    cmd->redir_err = NULL;
    cmd->redir_err_to_out = 0;

    int j = 0;
    for (int i = 0; i < cmd->argc; i++) {
        if (strcmp(cmd->argv[i], "<") == 0 && i + 1 < cmd->argc) {
            cmd->redir_in = cmd->argv[++i];
        } else if (strcmp(cmd->argv[i], ">>") == 0 && i + 1 < cmd->argc) {
            cmd->redir_append = cmd->argv[++i];
        } else if (strcmp(cmd->argv[i], ">") == 0 && i + 1 < cmd->argc) {
            cmd->redir_out = cmd->argv[++i];
        } else if (strcmp(cmd->argv[i], "2>&1") == 0) {
            cmd->redir_err_to_out = 1;
        } else if (strcmp(cmd->argv[i], "2>") == 0 && i + 1 < cmd->argc) {
            cmd->redir_err = cmd->argv[++i];
        } else {
            cmd->argv[j++] = cmd->argv[i];
        }
    }
    cmd->argc = j;
    cmd->argv[j] = NULL;

    return cmd->argc;
}

/* -----------------------------------------------------------------------
 * Glob pattern matching
 * ---------------------------------------------------------------------- */
int glob_match(const char *pat, const char *name) {
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return 1;
            while (*name) {
                if (glob_match(pat, name))
                    return 1;
                name++;
            }
            return 0;
        } else if (*pat == '?') {
            if (!*name) return 0;
            pat++;
            name++;
        } else {
            if (*pat != *name) return 0;
            pat++;
            name++;
        }
    }
    return *name == '\0';
}

#define GLOB_MAX_MATCHES 64
#define GLOB_STRBUF_SZ   4096

static char glob_strbuf[GLOB_STRBUF_SZ];
static int  glob_strbuf_pos;

static char *glob_strdup(const char *s) {
    int len = strlen(s) + 1;
    if (glob_strbuf_pos + len > GLOB_STRBUF_SZ)
        return NULL;
    char *p = glob_strbuf + glob_strbuf_pos;
    memcpy(p, s, len);
    glob_strbuf_pos += len;
    return p;
}

static int str_ptr_cmp(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static void glob_expand_token(const char *token, struct command *cmd) {
    if (!strchr(token, '*') && !strchr(token, '?')) {
        if (cmd->argc < MAX_ARGS - 1)
            cmd->argv[cmd->argc++] = (char *)token;
        return;
    }

    static char dirpart[512];
    static char basepart[256];
    const char *slash = strrchr(token, '/');
    if (slash) {
        int dlen = slash - token;
        if (dlen == 0) {
            dirpart[0] = '/';
            dirpart[1] = '\0';
        } else {
            memcpy(dirpart, token, dlen);
            dirpart[dlen] = '\0';
        }
        strncpy(basepart, slash + 1, 255);
        basepart[255] = '\0';
    } else {
        strcpy(dirpart, ".");
        strncpy(basepart, token, 255);
        basepart[255] = '\0';
    }

    DIR *d = opendir(dirpart);
    if (!d) {
        if (cmd->argc < MAX_ARGS - 1)
            cmd->argv[cmd->argc++] = (char *)token;
        return;
    }

    static const char *matches[GLOB_MAX_MATCHES];
    int nmatches = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && nmatches < GLOB_MAX_MATCHES) {
        if (de->d_name[0] == '.' && basepart[0] != '.')
            continue;
        if (!glob_match(basepart, de->d_name))
            continue;

        static char fullpath[512];
        if (strcmp(dirpart, ".") == 0) {
            snprintf(fullpath, sizeof(fullpath), "%s", de->d_name);
        } else if (dirpart[0] == '/' && dirpart[1] == '\0') {
            snprintf(fullpath, sizeof(fullpath), "/%s", de->d_name);
        } else {
            snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpart, de->d_name);
        }

        char *s = glob_strdup(fullpath);
        if (s)
            matches[nmatches++] = s;
    }
    closedir(d);

    if (nmatches == 0) {
        if (cmd->argc < MAX_ARGS - 1)
            cmd->argv[cmd->argc++] = (char *)token;
        return;
    }

    qsort(matches, nmatches, sizeof(char *), str_ptr_cmp);

    for (int i = 0; i < nmatches && cmd->argc < MAX_ARGS - 1; i++)
        cmd->argv[cmd->argc++] = (char *)matches[i];
}

static void glob_expand_cmd(struct command *cmd) {
    int has_glob = 0;
    for (int i = 0; i < cmd->argc; i++) {
        if (strchr(cmd->argv[i], '*') || strchr(cmd->argv[i], '?')) {
            has_glob = 1;
            break;
        }
    }
    if (!has_glob) return;

    glob_strbuf_pos = 0;

    static char *orig[MAX_ARGS];
    int norig = cmd->argc;
    for (int i = 0; i < norig; i++)
        orig[i] = cmd->argv[i];

    cmd->argc = 0;
    for (int i = 0; i < norig; i++)
        glob_expand_token(orig[i], cmd);

    cmd->argv[cmd->argc] = NULL;
}

/* -----------------------------------------------------------------------
 * parse_pipeline: parse one pipeline segment (already split on ; && ||)
 * into a struct pipeline.  Returns 1 if any commands were found, else 0.
 * ---------------------------------------------------------------------- */
static int parse_pipeline(char *seg, struct pipeline *pl) {
    pl->ncmds = 0;
    pl->background = 0;
    pl->connector  = CONN_NONE;

    /* strip trailing whitespace */
    int len = strlen(seg);
    while (len > 0 && (seg[len-1] == ' ' || seg[len-1] == '\t'))
        seg[--len] = '\0';

    /* trailing & means background */
    if (len > 0 && seg[len-1] == '&') {
        pl->background = 1;
        seg[--len] = '\0';
        while (len > 0 && (seg[len-1] == ' ' || seg[len-1] == '\t'))
            seg[--len] = '\0';
    }

    /* split on unquoted | */
    char *p = seg;
    while (p && pl->ncmds < MAX_PIPES) {
        char *pipe_pos = NULL;
        /* find unquoted | (but not ||) */
        char *scan = p;
        int in_dq = 0, in_sq = 0;
        while (*scan) {
            if (*scan == '"'  && !in_sq) { in_dq = !in_dq; scan++; continue; }
            if (*scan == '\'' && !in_dq) { in_sq = !in_sq; scan++; continue; }
            if (!in_dq && !in_sq && *scan == '|' && *(scan+1) != '|') {
                pipe_pos = scan;
                break;
            }
            scan++;
        }
        if (pipe_pos)
            *pipe_pos = '\0';
        if (tokenize(p, &pl->cmds[pl->ncmds]) > 0) {
            glob_expand_cmd(&pl->cmds[pl->ncmds]);
            pl->ncmds++;
        }
        p = pipe_pos ? pipe_pos + 1 : NULL;
    }

    return pl->ncmds > 0;
}

/* -----------------------------------------------------------------------
 * parse_line: split the line on unquoted ;, &&, || and fill a cmdlist.
 * Returns number of pipelines parsed (0 = empty line).
 * ---------------------------------------------------------------------- */
int parse_line(char *line, struct cmdlist *cl) {
    cl->npl = 0;

    /* strip comment: # at start of line or preceded by whitespace, but not $# */
    {
        char *p = line;
        int in_dq = 0, in_sq = 0;
        while (*p) {
            if (*p == '\'' && !in_dq) { in_sq = !in_sq; p++; continue; }
            if (*p == '"'  && !in_sq) { in_dq = !in_dq; p++; continue; }
            if (!in_sq && !in_dq && *p == '#' && (p == line || *(p-1) != '$')) {
                /* Only treat as comment if preceded by whitespace or at start */
                if (p == line || *(p-1) == ' ' || *(p-1) == '\t') {
                    *p = '\0';
                    break;
                }
            }
            p++;
        }
    }

    /* strip trailing whitespace / newline */
    int len = strlen(line);
    while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t' ||
                        line[len-1] == '\n' || line[len-1] == '\r'))
        line[--len] = '\0';

    if (len == 0)
        return 0;

    /*
     * Scan for unquoted ;, &&, ||.
     * We build a list of (start, connector) pairs.
     */
    char *seg_start = line;
    char *p = line;
    int   in_dq = 0, in_sq = 0;

    while (*p && cl->npl < MAX_CMDLIST) {
        if (*p == '"'  && !in_sq) { in_dq = !in_dq; p++; continue; }
        if (*p == '\'' && !in_dq) { in_sq = !in_sq; p++; continue; }
        if (in_dq || in_sq) { p++; continue; }

        int connector = CONN_NONE;
        int skip = 0;

        if (*p == '&' && *(p+1) == '&') {
            connector = CONN_AND; skip = 2;
        } else if (*p == '|' && *(p+1) == '|') {
            connector = CONN_OR;  skip = 2;
        } else if (*p == ';') {
            connector = CONN_SEQ; skip = 1;
        }

        if (connector != CONN_NONE) {
            *p = '\0';  /* terminate segment */
            struct pipeline *pl = &cl->pl[cl->npl];
            strncpy(pl->raw, seg_start, MAX_LINE - 1);
            pl->raw[MAX_LINE - 1] = '\0';
            pl->connector = connector;
            pl->ncmds = 0;
            cl->npl++;
            p += skip;
            /* skip leading spaces after separator */
            while (*p == ' ' || *p == '\t') p++;
            seg_start = p;
        } else {
            p++;
        }
    }

    /* last (or only) segment */
    if (*seg_start && cl->npl < MAX_CMDLIST) {
        struct pipeline *pl = &cl->pl[cl->npl];
        strncpy(pl->raw, seg_start, MAX_LINE - 1);
        pl->raw[MAX_LINE - 1] = '\0';
        pl->connector = CONN_NONE;
        pl->ncmds = 0;
        cl->npl++;
    }

    return cl->npl;
}

int expand_and_parse_pipeline(char *seg, struct pipeline *pl) {
    expand_vars(seg);
    return parse_pipeline(seg, pl);
}
