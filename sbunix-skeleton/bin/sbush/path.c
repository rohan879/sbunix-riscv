#include "sbush.h"
#include <fcntl.h>

static char path_buf[MAX_LINE];

char *resolve_path(const char *cmd) {
    int cmd_len = strlen(cmd);

    /* absolute or relative path — return as-is */
    if (strchr(cmd, '/')) {
        if (cmd_len >= MAX_LINE)
            return NULL;
        strncpy(path_buf, cmd, MAX_LINE - 1);
        path_buf[MAX_LINE - 1] = '\0';
        return path_buf;
    }

    char *path = var_get("PATH");
    if (!path)
        path = "/bin";

    char *p = path;
    while (*p) {
        char *colon = strchr(p, ':');
        int dlen;
        if (colon)
            dlen = colon - p;
        else
            dlen = strlen(p);

        if (dlen == 0) {
            if (cmd_len >= MAX_LINE)
                goto next;
            strncpy(path_buf, cmd, MAX_LINE - 1);
            path_buf[MAX_LINE - 1] = '\0';
        } else {
            /* Check: dlen + 1 (slash) + cmd_len + 1 (NUL) must fit */
            if (dlen + 1 + cmd_len + 1 > MAX_LINE)
                goto next;
            memcpy(path_buf, p, dlen);
            path_buf[dlen] = '/';
            memcpy(path_buf + dlen + 1, cmd, cmd_len);
            path_buf[dlen + 1 + cmd_len] = '\0';
        }

        /* probe: try open, close to check existence */
        int fd = open(path_buf, 0);
        if (fd >= 0) {
            close(fd);
            return path_buf;
        }

    next:
        if (colon)
            p = colon + 1;
        else
            break;
    }
    return NULL;
}
