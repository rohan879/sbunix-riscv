#include "sbush.h"
#include <stdlib.h>

void execute_line(char *line) {
    /* static: struct cmdlist is ~130 KB; placing it on the stack overflows
     * the 32 KB user stack.  The shell is single-threaded so static is safe. */
    static struct cmdlist cl;
    if (parse_line(line, &cl) > 0)
        execute_cmdlist(&cl);
}

/* Read a line from stdin with echo and line editing.
 * Prints the prompt (ps1) before reading.  Handles Ctrl-C by printing a
 * fresh prompt and restarting the line, so callers never see a -2 return.
 * Returns the number of characters in buf (excluding NUL), or -1 on EOF. */
static int read_line(char *buf, int max, const char *ps1) {
restart:
    write(STDOUT_FILENO, ps1, strlen(ps1));
    int i = 0;
    while (i < max - 1) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0)
            return -1;

        if (c == '\x03') {
            /* Ctrl-C: print "^C\n" here (kernel may not have, e.g. when input
             * arrives via the direct UART poll path rather than the interrupt
             * handler).  Move to a fresh line then restart the prompt. */
            write(STDOUT_FILENO, "^C\n", 3);
            i = 0;
            goto restart;
        }

        if (c == '\x7f' || c == '\x08') {
            /* Backspace: erase last typed character; i > 0 prevents
             * erasing past the start of input (i.e. into the prompt). */
            if (i > 0) {
                i--;
                write(STDOUT_FILENO, "\x08 \x08", 3);
            }
            continue;
        }

        if (c == '\n') {
            write(STDOUT_FILENO, "\n", 1);
            buf[i] = '\0';
            return i;
        }

        /* Echo and store printable characters */
        write(STDOUT_FILENO, &c, 1);
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
        if (!ps1) ps1 = "sbush$ ";
        int r = read_line(line, sizeof(line), ps1);
        if (r == -1)
            break;       /* EOF */
        execute_line(line);
    }

    return 0;
}
