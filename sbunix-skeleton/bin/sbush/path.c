#include "sbush.h"

static char path_buf[MAX_LINE];

char *resolve_path(const char *cmd) {
    /* absolute or relative path — return as-is */
    if (strchr(cmd, '/')) {
        strcpy(path_buf, cmd);
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
            strcpy(path_buf, cmd);
        } else {
            strncpy(path_buf, p, dlen);
            path_buf[dlen] = '/';
            strcpy(path_buf + dlen + 1, cmd);
        }

        /* probe: try open, close to check existence */
        int fd = open(path_buf, 0);
        if (fd >= 0) {
            close(fd);
            return path_buf;
        }

        if (colon)
            p = colon + 1;
        else
            break;
    }
    return NULL;
}
