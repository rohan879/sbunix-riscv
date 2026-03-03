#include "sbush.h"
#include <stdlib.h>

void execute_line(char *line) {
    struct pipeline pl;
    if (parse_line(line, &pl) > 0)
        execute_pipeline(&pl);
}

static int read_line(char *buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0)
            return -1;
        if (c == '\n') {
            buf[i] = '\0';
            return i;
        }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

static void reap_zombies(void) {
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

int main(int argc, char *argv[], char *envp[]) {
    env_init(envp);

    /* set defaults */
    if (!var_get("PATH"))
        var_set("PATH", "/bin");
    if (!var_get("PS1"))
        var_set("PS1", "sbush$ ");
    var_export("PATH");

    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)))
        var_set("PWD", cwd);

    /* script mode */
    if (argc >= 2)
        return run_script(argv[1]);

    /* interactive REPL */
    char line[MAX_LINE];
    while (1) {
        reap_zombies();
        char *ps1 = var_get("PS1");
        if (ps1)
            write(STDOUT_FILENO, ps1, strlen(ps1));
        if (read_line(line, sizeof(line)) < 0)
            break;
        execute_line(line);
    }

    return 0;
}
