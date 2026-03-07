#include "sbush.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>

static char expand_buf[MAX_LINE];

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
            cmd->argv[cmd->argc++] = p;
            while (*p && *p != '"')
                p++;
            if (*p == '"')
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
    return cmd->argc;
}

/* -----------------------------------------------------------------------
 * Glob pattern matching
 * ---------------------------------------------------------------------- */
static int glob_match(const char *pat, const char *name) {
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

    char dirpart[512];
    char basepart[256];
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

    const char *matches[GLOB_MAX_MATCHES];
    int nmatches = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && nmatches < GLOB_MAX_MATCHES) {
        if (de->d_name[0] == '.' && basepart[0] != '.')
            continue;
        if (!glob_match(basepart, de->d_name))
            continue;

        char fullpath[512];
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

    char *orig[MAX_ARGS];
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
        int in_q = 0;
        while (*scan) {
            if (*scan == '"') { in_q = !in_q; scan++; continue; }
            if (!in_q && *scan == '|' && *(scan+1) != '|') {
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

    /* strip comment */
    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';

    /* strip trailing whitespace / newline */
    int len = strlen(line);
    while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t' ||
                        line[len-1] == '\n' || line[len-1] == '\r'))
        line[--len] = '\0';

    if (len == 0)
        return 0;

    /* expand variables */
    expand_vars(line);
    len = strlen(line);
    if (len == 0)
        return 0;

    /*
     * Scan for unquoted ;, &&, ||.
     * We build a list of (start, connector) pairs.
     */
    char *seg_start = line;
    char *p = line;
    int   in_q = 0;

    while (*p && cl->npl < MAX_CMDLIST) {
        if (*p == '"') { in_q = !in_q; p++; continue; }
        if (in_q)       { p++; continue; }

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
            if (parse_pipeline(seg_start, pl)) {
                pl->connector = connector;
                cl->npl++;
            }
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
        if (parse_pipeline(seg_start, pl)) {
            pl->connector = CONN_NONE;
            cl->npl++;
        }
    }

    return cl->npl;
}
