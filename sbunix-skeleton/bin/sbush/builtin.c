#include "sbush.h"

int is_builtin(const char *cmd) {
    return (strcmp(cmd, "cd") == 0 ||
            strcmp(cmd, "export") == 0 ||
            strcmp(cmd, "set") == 0 ||
            strcmp(cmd, "exit") == 0);
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

    return 1;
}
