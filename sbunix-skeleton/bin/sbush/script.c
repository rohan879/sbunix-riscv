#include "sbush.h"

int run_script(const char *path) {
    int fd = open(path, 0);
    if (fd < 0) {
        printf("sbush: %s: No such file or directory\n", path);
        return 1;
    }

    int first_line = 1;
    char line[MAX_LINE];
    int i = 0;
    char ch;
    ssize_t n;

    while ((n = read(fd, &ch, 1)) > 0) {
        if (ch != '\n' && i < MAX_LINE - 1) {
            line[i++] = ch;
            continue;
        }
        line[i] = '\0';

        if (first_line) {
            first_line = 0;
            if (line[0] == '#' && line[1] == '!') {
                i = 0;
                continue;
            }
        }

        /* BF: Always call execute_line when heredoc, control flow, or function
         * definition is active — blank lines are meaningful content in those
         * contexts (e.g. blank line inside a heredoc body). */
        if (i > 0 || cf_active() || heredoc_active() || func_def_active())
            execute_line(line);
        i = 0;
    }

    if (i > 0) {
        line[i] = '\0';
        if (!(first_line && line[0] == '#' && line[1] == '!') &&
            (i > 0 || cf_active() || heredoc_active() || func_def_active()))
            execute_line(line);
    }

    close(fd);

    return last_exit_status;
}
