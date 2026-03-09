#include "sbush.h"
#include <sys/stat.h>

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
            strcmp(cmd, "return") == 0);
}

int run_builtin(struct command *cmd) {
    if (strcmp(cmd->argv[0], "cd") == 0) {
        const char *dir = cmd->argc > 1 ? cmd->argv[1] : var_get("HOME");
        if (!dir)
            dir = "/";
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
        for (int i = 1; i < cmd->argc; i++) {
            if (i > 1) write(STDOUT_FILENO, " ", 1);
            write(STDOUT_FILENO, cmd->argv[i], strlen(cmd->argv[i]));
        }
        write(STDOUT_FILENO, "\n", 1);
        return 0;
    }

    if (strcmp(cmd->argv[0], "exit") == 0) {
        int code = 0;
        if (cmd->argc > 1) {
            /* simple atoi */
            const char *s = cmd->argv[1];
            int neg = 0;
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
        struct job *j = (id > 0) ? job_find(id) : NULL;
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

    if (strcmp(cmd->argv[0], "bg") == 0) {
        int id = cmd->argc > 1 ? atoi(cmd->argv[1]) : -1;
        struct job *j = (id > 0) ? job_find(id) : NULL;
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

    return 1;
}
