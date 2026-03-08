#include "sbush.h"

int is_builtin(const char *cmd) {
    return (strcmp(cmd, "cd") == 0 ||
            strcmp(cmd, "export") == 0 ||
            strcmp(cmd, "set") == 0 ||
            strcmp(cmd, "unset") == 0 ||
            strcmp(cmd, "shift") == 0 ||
            strcmp(cmd, "exit") == 0 ||
            strcmp(cmd, "jobs") == 0 ||
            strcmp(cmd, "fg") == 0 ||
            strcmp(cmd, "bg") == 0);
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
