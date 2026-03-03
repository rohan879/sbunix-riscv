#include "sbush.h"

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
        if (tokenize(seg, &pl->cmds[pl->ncmds]) > 0)
            pl->ncmds++;
        seg = pipe_pos ? pipe_pos + 1 : NULL;
    }

    return pl->ncmds;
}
