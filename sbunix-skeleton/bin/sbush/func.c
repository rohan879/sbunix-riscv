#include "sbush.h"

#define MAX_FUNCS     8
#define FUNC_BODY_SZ  2048

struct shell_func {
    char name[64];
    char body[FUNC_BODY_SZ];
    int  body_len;
};

static struct shell_func func_table[MAX_FUNCS];
static int num_funcs = 0;

int func_return_flag = 0;

/* -----------------------------------------------------------------------
 * Function definition accumulation state
 * ---------------------------------------------------------------------- */
static int  fdef_active = 0;
static char fdef_name[64];
static char fdef_body[FUNC_BODY_SZ];
static int  fdef_body_len = 0;
static int  fdef_brace_depth = 0;

int func_def_active(void) { return fdef_active; }

/* Append a line to a NUL-separated body buffer */
static int func_append(char *buf, int *len, int maxlen, const char *line) {
    int slen = strlen(line);
    if (*len + slen + 1 >= maxlen)
        return -1;
    memcpy(buf + *len, line, slen);
    *len += slen;
    buf[*len] = '\0';
    (*len)++;
    return 0;
}

/* -----------------------------------------------------------------------
 * Lookup / define / call
 * ---------------------------------------------------------------------- */
struct shell_func *func_find(const char *name) {
    for (int i = 0; i < num_funcs; i++)
        if (strcmp(func_table[i].name, name) == 0)
            return &func_table[i];
    return NULL;
}

static int func_store(const char *name, const char *body, int body_len) {
    struct shell_func *f = func_find(name);
    if (!f) {
        if (num_funcs >= MAX_FUNCS) {
            dprintf(2, "sbush: too many functions\n");
            return -1;
        }
        f = &func_table[num_funcs++];
    }
    strncpy(f->name, name, 63);
    f->name[63] = '\0';
    if (body_len > FUNC_BODY_SZ - 1) body_len = FUNC_BODY_SZ - 1;
    memcpy(f->body, body, body_len);
    f->body_len = body_len;
    return 0;
}

/* Save/restore positional parameters for function calls (stacked) */
#define FUNC_SAVE_ARGS   8
#define FUNC_SAVE_LEVELS 4
static char saved_pos[FUNC_SAVE_LEVELS][FUNC_SAVE_ARGS][256];
static char saved_zero[FUNC_SAVE_LEVELS][256];
static int  saved_count[FUNC_SAVE_LEVELS];
static int  save_depth = 0;

void func_reset_depth(void) { save_depth = 0; }

int func_call(const char *name, int argc, char **argv) {
    struct shell_func *f = func_find(name);
    if (!f) return -1;

    if (save_depth >= FUNC_SAVE_LEVELS) {
        dprintf(2, "sbush: function call nesting too deep\n");
        return 1;
    }

    int d = save_depth;

    /* Save positional parameters */
    saved_count[d] = positional_count;
    strncpy(saved_zero[d], positional_get(0), 255);
    saved_zero[d][255] = '\0';
    for (int i = 0; i < positional_count && i < FUNC_SAVE_ARGS; i++) {
        char *p = positional_get(i + 1);
        if (p) strncpy(saved_pos[d][i], p, 255);
        else saved_pos[d][i][0] = '\0';
        saved_pos[d][i][255] = '\0';
    }
    save_depth++;

    /* Set new positional parameters (skip function name) */
    if (argc > 1)
        positional_set_args(argc - 1, argv + 1);
    else
        positional_set_args(0, NULL);

    /* Execute function body (NUL-separated lines) */
    static char func_tmp[MAX_LINE];
    int pos = 0;
    func_return_flag = 0;
    while (pos < f->body_len && !func_return_flag) {
        int i = 0;
        while (pos < f->body_len && f->body[pos] != '\0' && i < MAX_LINE - 1)
            func_tmp[i++] = f->body[pos++];
        func_tmp[i] = '\0';
        if (pos < f->body_len) pos++;
        if (i > 0)
            execute_line(func_tmp);
    }
    func_return_flag = 0;
    int ret = last_exit_status;

    /* Restore positional parameters */
    save_depth--;
    d = save_depth;
    if (saved_count[d] > 0) {
        static char *rargv[FUNC_SAVE_ARGS];
        for (int i = 0; i < saved_count[d] && i < FUNC_SAVE_ARGS; i++)
            rargv[i] = saved_pos[d][i];
        positional_set_args(saved_count[d], rargv);
    } else {
        positional_set_args(0, NULL);
    }
    positional_set_zero(saved_zero[d]);

    return ret;
}

/* -----------------------------------------------------------------------
 * Handle function definition lines (called from execute_line)
 * Returns 1 if the line was consumed, 0 if not.
 * ---------------------------------------------------------------------- */
int func_handle_line(char *line) {
    /* If actively accumulating a function body */
    if (fdef_active) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        /* Track brace nesting */
        char *scan = line;
        while (*scan) {
            if (*scan == '{') fdef_brace_depth++;
            else if (*scan == '}') fdef_brace_depth--;
            scan++;
        }

        if (fdef_brace_depth <= 0) {
            /* Closing brace — end of function definition */
            /* Don't include the closing } line in the body.
             * But if there's content before }, include it. */
            if (p[0] != '}') {
                char *brace = strrchr(line, '}');
                if (brace) {
                    *brace = '\0';
                    char *trimmed = line;
                    while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
                    if (*trimmed)
                        func_append(fdef_body, &fdef_body_len, FUNC_BODY_SZ, trimmed);
                }
            }
            func_store(fdef_name, fdef_body, fdef_body_len);
            fdef_active = 0;
        } else {
            func_append(fdef_body, &fdef_body_len, FUNC_BODY_SZ, line);
        }
        return 1;
    }

    /* Check for function definition: NAME() or NAME () */
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;

    char name[64];
    int ni = 0;
    while (is_name_char(*p) && ni < 63)
        name[ni++] = *p++;
    name[ni] = '\0';
    if (ni == 0) return 0;

    while (*p == ' ' || *p == '\t') p++;
    if (*p != '(' || *(p+1) != ')')
        return 0;
    p += 2; /* skip () */
    while (*p == ' ' || *p == '\t') p++;

    /* Must have { to start function body */
    if (*p == '{') {
        p++;
        while (*p == ' ' || *p == '\t') p++;
        strncpy(fdef_name, name, 63);
        fdef_name[63] = '\0';
        fdef_body_len = 0;
        fdef_brace_depth = 1;

        /* Check for single-line function: name() { body; } */
        char *close = strrchr(p, '}');
        if (close && close > p) {
            *close = '\0';
            char *trimmed = p;
            while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

            /* Split on ; and store as NUL-separated body */
            char *sp = trimmed;
            while (*sp) {
                char *seg = sp;
                while (*sp && *sp != ';') sp++;
                if (*sp == ';') *sp++ = '\0';
                while (*seg == ' ' || *seg == '\t') seg++;
                if (*seg)
                    func_append(fdef_body, &fdef_body_len, FUNC_BODY_SZ, seg);
                while (*sp == ' ' || *sp == '\t') sp++;
            }
            func_store(fdef_name, fdef_body, fdef_body_len);
            return 1;
        }

        /* Multi-line: start accumulating */
        if (*p) {
            func_append(fdef_body, &fdef_body_len, FUNC_BODY_SZ, p);
        }
        fdef_active = 1;
        return 1;
    }

    return 0;
}
