#include "sbush.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>

static char expand_buf[MAX_LINE];

static void expand_vars(char *line) {
    char *src = line;
    char *dst = expand_buf;
    char *end = expand_buf + MAX_LINE - 1;

    while (*src && dst < end) {
        if (*src == '$') {
            src++;
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
 * Glob pattern matching: match a single filename component against a
 * pattern that may contain '*' (matches any sequence of chars) and '?'
 * (matches exactly one char).  No '/' in either string.
 * ---------------------------------------------------------------------- */
static int glob_match(const char *pat, const char *name) {
    while (*pat) {
        if (*pat == '*') {
            /* skip consecutive stars */
            while (*pat == '*') pat++;
            if (!*pat) return 1;          /* trailing * matches anything */
            /* try each possible match position */
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

/* -----------------------------------------------------------------------
 * Expand a single glob token (possibly containing '*' or '?').
 *
 * Strategy:
 *   - Split the pattern into dir-part and base-part at the last '/'.
 *   - opendir the dir-part, iterate entries, match each against base-part.
 *   - Sort matches and append them to cmd->argv[].
 *   - If no matches are found, leave the original token unchanged (POSIX
 *     "no match" behaviour = pass through literally).
 *
 * All matched strings are appended into the static glob_strbuf pool so
 * they have stable addresses for argv[].
 * ---------------------------------------------------------------------- */

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

/* Compare function for qsort on char* array */
static int str_ptr_cmp(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static void glob_expand_token(const char *token, struct command *cmd) {
    /* Only bother if the token contains a wildcard */
    if (!strchr(token, '*') && !strchr(token, '?')) {
        if (cmd->argc < MAX_ARGS - 1)
            cmd->argv[cmd->argc++] = (char *)token;
        return;
    }

    /* Split into dir and base */
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
        /* pass through literally */
        if (cmd->argc < MAX_ARGS - 1)
            cmd->argv[cmd->argc++] = (char *)token;
        return;
    }

    const char *matches[GLOB_MAX_MATCHES];
    int nmatches = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && nmatches < GLOB_MAX_MATCHES) {
        /* skip hidden files unless pattern starts with '.' */
        if (de->d_name[0] == '.' && basepart[0] != '.')
            continue;
        if (!glob_match(basepart, de->d_name))
            continue;

        /* build full path */
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
        /* no matches — pass through literally */
        if (cmd->argc < MAX_ARGS - 1)
            cmd->argv[cmd->argc++] = (char *)token;
        return;
    }

    /* sort matches */
    qsort(matches, nmatches, sizeof(char *), str_ptr_cmp);

    for (int i = 0; i < nmatches && cmd->argc < MAX_ARGS - 1; i++)
        cmd->argv[cmd->argc++] = (char *)matches[i];
}

/* -----------------------------------------------------------------------
 * Expand all glob tokens in a command's argv[].
 * ---------------------------------------------------------------------- */
static void glob_expand_cmd(struct command *cmd) {
    /* Check if any token has a wildcard; if not, skip entirely */
    int has_glob = 0;
    for (int i = 0; i < cmd->argc; i++) {
        if (strchr(cmd->argv[i], '*') || strchr(cmd->argv[i], '?')) {
            has_glob = 1;
            break;
        }
    }
    if (!has_glob) return;

    /* Reset glob string pool */
    glob_strbuf_pos = 0;

    /* Save original argv */
    char *orig[MAX_ARGS];
    int norig = cmd->argc;
    for (int i = 0; i < norig; i++)
        orig[i] = cmd->argv[i];

    /* Rebuild argv with expansions */
    cmd->argc = 0;
    for (int i = 0; i < norig; i++)
        glob_expand_token(orig[i], cmd);

    cmd->argv[cmd->argc] = NULL;
}

int parse_line(char *line, struct pipeline *pl) {
    /* strip comment */
    char *hash = strchr(line, '#');
    if (hash)
        *hash = '\0';

    /* strip trailing whitespace/newline */
    int len = strlen(line);
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' ||
                        line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';

    if (len == 0)
        return 0;

    /* expand variables */
    expand_vars(line);
    len = strlen(line);

    /* check for background */
    pl->background = 0;
    if (len > 0 && line[len - 1] == '&') {
        pl->background = 1;
        line[--len] = '\0';
        while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t'))
            line[--len] = '\0';
    }

    /* split on pipes */
    pl->ncmds = 0;
    char *seg = line;
    while (seg && pl->ncmds < MAX_PIPES) {
        char *pipe_pos = strchr(seg, '|');
        if (pipe_pos)
            *pipe_pos = '\0';
        if (tokenize(seg, &pl->cmds[pl->ncmds]) > 0) {
            /* expand globs for this command */
            glob_expand_cmd(&pl->cmds[pl->ncmds]);
            pl->ncmds++;
        }
        seg = pipe_pos ? pipe_pos + 1 : NULL;
    }

    return pl->ncmds;
}
