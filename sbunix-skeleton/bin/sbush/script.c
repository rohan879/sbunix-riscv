#include "sbush.h"

int run_script(const char *path) {
    int fd = open(path, 0);
    if (fd < 0) {
        printf("sbush: %s: No such file or directory\n", path);
        return 1;
    }

    char file_buf[65536];
    int total = 0;
    ssize_t n;
    while ((n = read(fd, file_buf + total, sizeof(file_buf) - total - 1)) > 0)
        total += n;
    close(fd);
    file_buf[total] = '\0';

    /* skip shebang line */
    char *p = file_buf;
    if (p[0] == '#' && p[1] == '!') {
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }

    /* execute each line */
    while (*p) {
        char line[MAX_LINE];
        int i = 0;
        while (*p && *p != '\n' && i < MAX_LINE - 1)
            line[i++] = *p++;
        line[i] = '\0';
        if (*p == '\n')
            p++;
        if (i > 0)
            execute_line(line);
    }

    return last_exit_status;
}
