#include "sbush.h"
#include <sys/stat.h>

/* -----------------------------------------------------------------------
 * Alias table
 * ---------------------------------------------------------------------- */
static struct alias_entry aliases[MAX_ALIASES];
static int nalias = 0;

char *alias_get(const char *name) {
    for (int i = 0; i < nalias; i++) {
        if (strcmp(aliases[i].name, name) == 0)
            return aliases[i].value;
    }
    return NULL;
}

void alias_set(const char *name, const char *value) {
    for (int i = 0; i < nalias; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            strncpy(aliases[i].value, value, 255);
            aliases[i].value[255] = '\0';
            return;
        }
    }
    if (nalias < MAX_ALIASES) {
        strncpy(aliases[nalias].name, name, 63);
        aliases[nalias].name[63] = '\0';
        strncpy(aliases[nalias].value, value, 255);
        aliases[nalias].value[255] = '\0';
        nalias++;
    }
}

void alias_unset(const char *name) {
    for (int i = 0; i < nalias; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            aliases[i] = aliases[--nalias];
            return;
        }
    }
}

void alias_list(void) {
    for (int i = 0; i < nalias; i++)
        printf("alias %s='%s'\n", aliases[i].name, aliases[i].value);
}

/* -----------------------------------------------------------------------
 * test / [ builtin — recursive descent evaluator
 * ---------------------------------------------------------------------- */
static char **t_argv;
static int    t_argc;
static int    t_pos;

static const char *t_peek(void) {
    if (t_pos < t_argc)
        return t_argv[t_pos];
    return NULL;
}

static const char *t_advance(void) {
    if (t_pos < t_argc)
        return t_argv[t_pos++];
    return NULL;
}

static long str_to_long(const char *s) {
    long val = 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9')
        val = val * 10 + (*s++ - '0');
    return neg ? -val : val;
}

static int eval_or(void);

static int eval_primary(void) {
    const char *tok = t_peek();
    if (!tok) return 0; /* false */

    /* parenthesized subexpression */
    if (strcmp(tok, "(") == 0) {
        t_advance();
        int r = eval_or();
        if (t_peek() && strcmp(t_peek(), ")") == 0)
            t_advance();
        return r;
    }

    /* unary operators */
    if (strcmp(tok, "!") == 0) {
        t_advance();
        return !eval_primary();
    }
    if (strcmp(tok, "-n") == 0) {
        t_advance();
        const char *s = t_advance();
        return s && s[0] != '\0';
    }
    if (strcmp(tok, "-z") == 0) {
        t_advance();
        const char *s = t_advance();
        return !s || s[0] == '\0';
    }

    /* file test operators */
    if (tok[0] == '-' && tok[1] != '\0' && tok[2] == '\0') {
        char op = tok[1];
        if (op == 'e' || op == 'f' || op == 'd' ||
            op == 'r' || op == 'w' || op == 'x') {
            t_advance();
            const char *path = t_advance();
            if (!path) return 0;
            struct stat st;
            if (stat(path, &st) < 0) return 0;
            switch (op) {
                case 'e': return 1;
                case 'f': return S_ISREG(st.st_mode);
                case 'd': return S_ISDIR(st.st_mode);
                case 'r': return (st.st_mode & S_IRUSR) != 0;
                case 'w': return (st.st_mode & S_IWUSR) != 0;
                case 'x': return (st.st_mode & S_IXUSR) != 0;
            }
            return 0;
        }
    }

    /* consume first operand */
    t_advance();
    const char *op = t_peek();
    if (!op) {
        /* bare string: true if non-empty */
        return tok[0] != '\0';
    }

    /* binary string operators */
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
        t_advance();
        const char *rhs = t_advance();
        return rhs && strcmp(tok, rhs) == 0;
    }
    if (strcmp(op, "!=") == 0) {
        t_advance();
        const char *rhs = t_advance();
        return !rhs || strcmp(tok, rhs) != 0;
    }

    /* binary integer operators */
    if (strcmp(op, "-eq") == 0) { t_advance(); const char *r = t_advance(); return r && str_to_long(tok) == str_to_long(r); }
    if (strcmp(op, "-ne") == 0) { t_advance(); const char *r = t_advance(); return r && str_to_long(tok) != str_to_long(r); }
    if (strcmp(op, "-lt") == 0) { t_advance(); const char *r = t_advance(); return r && str_to_long(tok) < str_to_long(r); }
    if (strcmp(op, "-le") == 0) { t_advance(); const char *r = t_advance(); return r && str_to_long(tok) <= str_to_long(r); }
    if (strcmp(op, "-gt") == 0) { t_advance(); const char *r = t_advance(); return r && str_to_long(tok) > str_to_long(r); }
    if (strcmp(op, "-ge") == 0) { t_advance(); const char *r = t_advance(); return r && str_to_long(tok) >= str_to_long(r); }

    /* bare string (non-empty = true) */
    return tok[0] != '\0';
}

static int eval_and(void) {
    int result = eval_primary();
    while (t_peek() && strcmp(t_peek(), "-a") == 0) {
        t_advance();
        int rhs = eval_primary();
        result = result && rhs;
    }
    return result;
}

static int eval_or(void) {
    int result = eval_and();
    while (t_peek() && strcmp(t_peek(), "-o") == 0) {
        t_advance();
        int rhs = eval_and();
        result = result || rhs;
    }
    return result;
}

static int builtin_test(struct command *cmd) {
    int is_bracket = (strcmp(cmd->argv[0], "[") == 0);

    t_argv = cmd->argv + 1;
    t_argc = cmd->argc - 1;

    /* [ requires closing ] */
    if (is_bracket) {
        if (t_argc == 0 || strcmp(t_argv[t_argc - 1], "]") != 0) {
            dprintf(2, "sbush: [: missing ]\n");
            return 2;
        }
        t_argc--; /* exclude ] */
    }

    if (t_argc == 0)
        return 1; /* no args = false */

    t_pos = 0;
    int result = eval_or();
    return result ? 0 : 1;
}

int is_builtin(const char *cmd) {
    return (strcmp(cmd, "cd") == 0 ||
            strcmp(cmd, "echo") == 0 ||
            strcmp(cmd, "export") == 0 ||
            strcmp(cmd, "set") == 0 ||
            strcmp(cmd, "unset") == 0 ||
            strcmp(cmd, "shift") == 0 ||
            strcmp(cmd, "test") == 0 ||
            strcmp(cmd, "[") == 0 ||
            strcmp(cmd, "true") == 0 ||
            strcmp(cmd, "false") == 0 ||
            strcmp(cmd, "exit") == 0 ||
            strcmp(cmd, "jobs") == 0 ||
            strcmp(cmd, "fg") == 0 ||
            strcmp(cmd, "bg") == 0 ||
            strcmp(cmd, "return") == 0 ||
            strcmp(cmd, "source") == 0 ||
            strcmp(cmd, ".") == 0 ||
            strcmp(cmd, "eval") == 0 ||
            strcmp(cmd, "read") == 0 ||
            strcmp(cmd, "trap") == 0 ||
            strcmp(cmd, "break") == 0 ||
            strcmp(cmd, "continue") == 0 ||
            strcmp(cmd, "clear") == 0 ||
            strcmp(cmd, "which") == 0 ||
            strcmp(cmd, "alias") == 0 ||
            strcmp(cmd, "unalias") == 0 ||
            strcmp(cmd, "exec") == 0 ||
            strcmp(cmd, "umask") == 0 ||
            strcmp(cmd, "pwd") == 0 ||
            strcmp(cmd, ":") == 0 ||
            strcmp(cmd, "command") == 0 ||
            strcmp(cmd, "wait") == 0 ||
            strcmp(cmd, "type") == 0);
}

/* -----------------------------------------------------------------------
 * clear builtin — clear screen via ANSI escape
 * ---------------------------------------------------------------------- */
static int builtin_clear(struct command *cmd) {
    (void)cmd;
    write(STDOUT_FILENO, "\033[2J\033[H", 7);
    return 0;
}

/* -----------------------------------------------------------------------
 * which builtin — locate a command
 * ---------------------------------------------------------------------- */
static int builtin_which(struct command *cmd) {
    if (cmd->argc < 2) {
        dprintf(2, "sbush: which: argument required\n");
        return 1;
    }

    int ret = 0;
    for (int i = 1; i < cmd->argc; i++) {
        const char *name = cmd->argv[i];

        /* Check if it's a shell builtin */
        if (is_builtin(name)) {
            dprintf(STDOUT_FILENO, "%s: shell builtin\n", name);
            continue;
        }

        /* Check if it's a shell function */
        if (func_find(name)) {
            dprintf(STDOUT_FILENO, "%s: shell function\n", name);
            continue;
        }

        /* Search $PATH for an executable */
        char *path = var_get("PATH");
        if (!path)
            path = "/bin";

        char buf[MAX_LINE];
        char *p = path;
        int found = 0;
        while (*p) {
            char *colon = strchr(p, ':');
            int dlen;
            if (colon)
                dlen = colon - p;
            else
                dlen = strlen(p);

            if (dlen == 0) {
                strcpy(buf, name);
            } else {
                strncpy(buf, p, dlen);
                buf[dlen] = '/';
                strcpy(buf + dlen + 1, name);
            }

            struct stat st;
            if (stat(buf, &st) == 0 && S_ISREG(st.st_mode) &&
                (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
                dprintf(STDOUT_FILENO, "%s\n", buf);
                found = 1;
                break;
            }

            if (colon)
                p = colon + 1;
            else
                break;
        }

        if (!found) {
            dprintf(2, "sbush: which: no %s in PATH\n", name);
            ret = 1;
        }
    }
    return ret;
}

int run_builtin(struct command *cmd) {
    if (strcmp(cmd->argv[0], "cd") == 0) {
        const char *dir = cmd->argc > 1 ? cmd->argv[1] : var_get("HOME");
        if (!dir)
            dir = "/";
        /* Handle "cd -": switch to OLDPWD */
        if (strcmp(dir, "-") == 0) {
            dir = var_get("OLDPWD");
            if (!dir) {
                printf("cd: OLDPWD not set\n");
                return 1;
            }
            printf("%s\n", dir);
        }
        /* Save current directory as OLDPWD before changing */
        char oldcwd[256];
        if (getcwd(oldcwd, sizeof(oldcwd)))
            var_set("OLDPWD", oldcwd);
        if (chdir(dir) < 0) {
            printf("cd: %s: No such file or directory\n", dir);
            return 1;
        }
        char cwd[256];
        if (getcwd(cwd, sizeof(cwd)))
            var_set("PWD", cwd);
        return 0;
    }

    if (strcmp(cmd->argv[0], "export") == 0) {
        for (int i = 1; i < cmd->argc; i++) {
            char *eq = strchr(cmd->argv[i], '=');
            if (eq) {
                *eq = '\0';
                var_set(cmd->argv[i], eq + 1);
                var_export(cmd->argv[i]);
                *eq = '=';
            } else {
                var_export(cmd->argv[i]);
            }
        }
        return 0;
    }

    if (strcmp(cmd->argv[0], "set") == 0) {
        if (cmd->argc == 1) return 0;
        /* Handle set -e/-x/-u/-- flags */
        if (cmd->argv[1][0] == '-' || cmd->argv[1][0] == '+') {
            int enable = (cmd->argv[1][0] == '-');
            for (const char *p = cmd->argv[1] + 1; *p; p++) {
                if (*p == 'e') set_flag_e = enable;
                else if (*p == 'x') set_flag_x = enable;
                else if (*p == 'u') set_flag_u = enable;
                else if (*p == '-') break; /* -- */
            }
            return 0;
        }
        if (cmd->argc == 3) {
            var_set(cmd->argv[1], cmd->argv[2]);
        }
        return 0;
    }

    if (strcmp(cmd->argv[0], "unset") == 0) {
        for (int i = 1; i < cmd->argc; i++)
            var_unset(cmd->argv[i]);
        return 0;
    }

    if (strcmp(cmd->argv[0], "shift") == 0) {
        int n = 1;
        if (cmd->argc > 1) {
            n = atoi(cmd->argv[1]);
            if (n < 0) n = 0;
        }
        if (n > positional_count) {
            dprintf(2, "sbush: shift: count exceeds positional parameters\n");
            return 1;
        }
        positional_shift(n);
        return 0;
    }

    if (strcmp(cmd->argv[0], "true") == 0) return 0;
    if (strcmp(cmd->argv[0], "false") == 0) return 1;

    if (strcmp(cmd->argv[0], "test") == 0 || strcmp(cmd->argv[0], "[") == 0)
        return builtin_test(cmd);

    if (strcmp(cmd->argv[0], "echo") == 0) {
        int start = 1;
        int no_newline = 0;
        if (cmd->argc > 1 && strcmp(cmd->argv[1], "-n") == 0) {
            no_newline = 1;
            start = 2;
        }
        for (int i = start; i < cmd->argc; i++) {
            if (i > start) write(STDOUT_FILENO, " ", 1);
            write(STDOUT_FILENO, cmd->argv[i], strlen(cmd->argv[i]));
        }
        if (!no_newline) write(STDOUT_FILENO, "\n", 1);
        return 0;
    }

    if (strcmp(cmd->argv[0], "exit") == 0) {
        int code = last_exit_status;
        if (cmd->argc > 1) {
            /* simple atoi */
            const char *s = cmd->argv[1];
            int neg = 0;
            code = 0;
            if (*s == '-') { neg = 1; s++; }
            while (*s >= '0' && *s <= '9')
                code = code * 10 + (*s++ - '0');
            if (neg) code = -code;
        }
        exit(code);
    }

    if (strcmp(cmd->argv[0], "jobs") == 0) {
        job_list();
        return 0;
    }

    if (strcmp(cmd->argv[0], "fg") == 0) {
        int id = cmd->argc > 1 ? atoi(cmd->argv[1]) : -1;
        struct job *j = (id > 0) ? job_find(id) : job_last();
        if (!j) {
            printf("fg: no such job\n");
            return 1;
        }
        /* Give terminal to the job's process group */
        tcsetpgrp(STDIN_FILENO, j->pid);
        /* Track as foreground for signal forwarding */
        fg_pid = j->pid;
        /* Resume the stopped process */
        kill(j->pid, SIGCONT);
        j->state = JOB_RUNNING;
        printf("%s\n", j->cmd);
        int status;
        waitpid(j->pid, &status, WUNTRACED);
        /* Reclaim terminal */
        fg_pid = 0;
        tcsetpgrp(STDIN_FILENO, getpgrp());
        if (WIFSTOPPED(status)) {
            j->state = JOB_STOPPED;
            printf("[%d] Stopped\t%s\n", j->id, j->cmd);
            return 128 + WSTOPSIG(status);
        }
        job_remove(j->pid);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }

    if (strcmp(cmd->argv[0], "return") == 0) {
        int code = 0;
        if (cmd->argc > 1) {
            const char *s = cmd->argv[1];
            int neg = 0;
            if (*s == '-') { neg = 1; s++; }
            while (*s >= '0' && *s <= '9')
                code = code * 10 + (*s++ - '0');
            if (neg) code = -code;
        }
        last_exit_status = code;
        func_return_flag = 1;
        return code;
    }

    if (strcmp(cmd->argv[0], "source") == 0 || strcmp(cmd->argv[0], ".") == 0) {
        if (cmd->argc < 2) {
            dprintf(2, "sbush: %s: filename argument required\n", cmd->argv[0]);
            return 2;
        }
        return run_script(cmd->argv[1]);
    }

    if (strcmp(cmd->argv[0], "break") == 0) {
        cf_break_count = cmd->argc > 1 ? atoi(cmd->argv[1]) : 1;
        if (cf_break_count < 1) cf_break_count = 1;
        return 0;
    }

    if (strcmp(cmd->argv[0], "continue") == 0) {
        cf_continue_flag = 1;
        return 0;
    }

    if (strcmp(cmd->argv[0], "trap") == 0) {
        /* trap 'cmd' SIGNAL or trap '' SIGNAL (reset) or trap - SIGNAL */
        if (cmd->argc < 3) {
            /* List traps or usage — just return OK for now */
            return 0;
        }
        const char *action = cmd->argv[1];
        for (int i = 2; i < cmd->argc; i++) {
            int sig = atoi(cmd->argv[i]);
            /* Also accept signal names */
            if (strcmp(cmd->argv[i], "INT") == 0) sig = SIGINT;
            else if (strcmp(cmd->argv[i], "TERM") == 0) sig = SIGTERM;
            else if (strcmp(cmd->argv[i], "QUIT") == 0) sig = SIGQUIT;
            else if (strcmp(cmd->argv[i], "EXIT") == 0) sig = 0;
            else if (strcmp(cmd->argv[i], "HUP") == 0) sig = SIGHUP;

            if (sig >= 0 && sig < 32)
                trap_set(sig, action);
        }
        return 0;
    }

    if (strcmp(cmd->argv[0], "read") == 0) {
        if (cmd->argc < 2) {
            dprintf(2, "sbush: read: variable name required\n");
            return 1;
        }
        static char rbuf[MAX_LINE];
        int ri = 0;
        char ch;
        while (ri < MAX_LINE - 1 && read(STDIN_FILENO, &ch, 1) == 1) {
            if (ch == '\n') break;
            rbuf[ri++] = ch;
        }
        rbuf[ri] = '\0';
        if (ri == 0 && ch != '\n')
            return 1; /* EOF */

        if (cmd->argc == 2) {
            /* Single variable gets entire line */
            var_set(cmd->argv[1], rbuf);
        } else {
            /* Multiple variables: split by whitespace, last gets remainder */
            char *p = rbuf;
            for (int i = 1; i < cmd->argc; i++) {
                while (*p == ' ' || *p == '\t') p++;
                if (i == cmd->argc - 1) {
                    /* Last variable gets the rest */
                    var_set(cmd->argv[i], p);
                } else {
                    char word[256];
                    int wi = 0;
                    while (*p && *p != ' ' && *p != '\t' && wi < 255)
                        word[wi++] = *p++;
                    word[wi] = '\0';
                    var_set(cmd->argv[i], word);
                }
            }
        }
        return 0;
    }

    if (strcmp(cmd->argv[0], "eval") == 0) {
        if (cmd->argc < 2)
            return 0;
        /* Concatenate all args into one string and execute */
        static char eval_buf[MAX_LINE];
        int len = 0;
        for (int i = 1; i < cmd->argc && len < MAX_LINE - 2; i++) {
            if (i > 1 && len < MAX_LINE - 1) eval_buf[len++] = ' ';
            const char *a = cmd->argv[i];
            while (*a && len < MAX_LINE - 1)
                eval_buf[len++] = *a++;
        }
        eval_buf[len] = '\0';
        execute_line(eval_buf);
        return last_exit_status;
    }

    if (strcmp(cmd->argv[0], "bg") == 0) {
        int id = cmd->argc > 1 ? atoi(cmd->argv[1]) : -1;
        struct job *j = (id > 0) ? job_find(id) : job_last();
        if (!j) {
            printf("bg: no such job\n");
            return 1;
        }
        /* Resume in background — no terminal change, no wait */
        kill(j->pid, SIGCONT);
        j->state = JOB_RUNNING;
        printf("[%d] %s &\n", j->id, j->cmd);
        return 0;
    }

    if (strcmp(cmd->argv[0], "clear") == 0)
        return builtin_clear(cmd);

    if (strcmp(cmd->argv[0], "which") == 0)
        return builtin_which(cmd);

    if (strcmp(cmd->argv[0], "umask") == 0) {
        if (cmd->argc == 1) {
            unsigned int m = umask(0);
            umask(m);
            printf("%04o\n", m);
            return 0;
        }
        const char *s = cmd->argv[1];
        unsigned int mask = 0;
        while (*s >= '0' && *s <= '7')
            mask = mask * 8 + (*s++ - '0');
        umask(mask);
        return 0;
    }

    if (strcmp(cmd->argv[0], "exec") == 0) {
        if (cmd->argc == 1) return 0;
        char *path = resolve_path(cmd->argv[1]);
        if (!path) {
            dprintf(2, "sbush: exec: %s: command not found\n", cmd->argv[1]);
            return 127;
        }
        char **envp = build_environ();
        execve(path, cmd->argv + 1, envp);
        dprintf(2, "sbush: exec: %s: exec failed\n", cmd->argv[1]);
        return 126;
    }

    if (strcmp(cmd->argv[0], "alias") == 0) {
        if (cmd->argc == 1) {
            alias_list();
            return 0;
        }
        for (int i = 1; i < cmd->argc; i++) {
            char *eq = strchr(cmd->argv[i], '=');
            if (eq) {
                *eq = '\0';
                alias_set(cmd->argv[i], eq + 1);
                *eq = '=';
            } else {
                char *val = alias_get(cmd->argv[i]);
                if (val)
                    printf("alias %s='%s'\n", cmd->argv[i], val);
                else
                    dprintf(2, "sbush: alias: %s: not found\n", cmd->argv[i]);
            }
        }
        return 0;
    }

    if (strcmp(cmd->argv[0], "unalias") == 0) {
        if (cmd->argc < 2) {
            dprintf(2, "sbush: unalias: usage: unalias name\n");
            return 1;
        }
        for (int i = 1; i < cmd->argc; i++)
            alias_unset(cmd->argv[i]);
        return 0;
    }

    if (strcmp(cmd->argv[0], "pwd") == 0) {
        char cwd[256];
        if (getcwd(cwd, sizeof(cwd))) {
            write(STDOUT_FILENO, cwd, strlen(cwd));
            write(STDOUT_FILENO, "\n", 1);
            return 0;
        }
        return 1;
    }

    if (strcmp(cmd->argv[0], ":") == 0) {
        return 0;
    }

    if (strcmp(cmd->argv[0], "command") == 0) {
        if (cmd->argc < 2) return 0;
        /* Skip functions/aliases; execute external command directly */
        char *path = resolve_path(cmd->argv[1]);
        if (!path) {
            dprintf(2, "%s: command not found\n", cmd->argv[1]);
            return 127;
        }
        char **envp = build_environ();
        pid_t pid = fork();
        if (pid == 0) {
            execve(path, cmd->argv + 1, envp);
            _exit(127);
        }
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        return 1;
    }

    if (strcmp(cmd->argv[0], "wait") == 0) {
        int status;
        if (cmd->argc > 1) {
            pid_t pid = (pid_t)atoi(cmd->argv[1]);
            waitpid(pid, &status, 0);
        } else {
            /* Wait for all background children */
            while (waitpid(-1, &status, 0) > 0)
                ;
        }
        return 0;
    }

    if (strcmp(cmd->argv[0], "type") == 0) {
        if (cmd->argc < 2) return 1;
        for (int i = 1; i < cmd->argc; i++) {
            if (is_builtin(cmd->argv[i]))
                printf("%s is a shell builtin\n", cmd->argv[i]);
            else if (func_find(cmd->argv[i]))
                printf("%s is a function\n", cmd->argv[i]);
            else {
                char *path = resolve_path(cmd->argv[i]);
                if (path)
                    printf("%s is %s\n", cmd->argv[i], path);
                else {
                    dprintf(2, "type: %s: not found\n", cmd->argv[i]);
                    return 1;
                }
            }
        }
        return 0;
    }

    return 1;
}
