#include "sbush.h"
#include <stdlib.h>

/* Run one pipeline; return its exit status (last command's status).
 * For background pipelines returns 0 immediately. */
int execute_pipeline(struct pipeline *pl) {
    if (pl->ncmds == 0)
        return 0;

    /* single builtin — run in-process (unless it's in a pipe) */
    if (pl->ncmds == 1 && !pl->background && is_builtin(pl->cmds[0].argv[0])) {
        return run_builtin(&pl->cmds[0]);
    }

    pid_t pids[MAX_PIPES];
    int prev_fd = -1;

    for (int i = 0; i < pl->ncmds; i++) {
        int pipefd[2] = {-1, -1};
        if (i < pl->ncmds - 1) {
            if (pipe(pipefd) < 0) {
                dprintf(2, "sbush: pipe failed\n");
                return 1;
            }
        }

        pid_t pid = fork();
        if (pid < 0) {
            dprintf(2, "sbush: fork failed\n");
            return 1;
        }

        if (pid == 0) {
            /* child */
            if (prev_fd >= 0) {
                dup2(prev_fd, STDIN_FILENO);
                close(prev_fd);
            }
            if (pipefd[1] >= 0) {
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }
            if (pipefd[0] >= 0)
                close(pipefd[0]);

            if (is_builtin(pl->cmds[i].argv[0])) {
                int ret = run_builtin(&pl->cmds[i]);
                exit(ret);
            }

            char *path = resolve_path(pl->cmds[i].argv[0]);
            if (!path) {
                dprintf(2, "sbush: %s: command not found\n", pl->cmds[i].argv[0]);
                exit(127);
            }
            char **envp = build_environ();
            execve(path, pl->cmds[i].argv, envp);
            dprintf(2, "sbush: %s: exec failed\n", pl->cmds[i].argv[0]);
            exit(126);
        }

        pids[i] = pid;
        if (prev_fd >= 0)
            close(prev_fd);
        if (pipefd[1] >= 0)
            close(pipefd[1]);
        prev_fd = pipefd[0];
    }

    if (pl->background) {
        /* don't wait; shell continues immediately */
        if (prev_fd >= 0) close(prev_fd);
        return 0;
    }

    /* wait for all processes; return exit status of last */
    int last_status = 0;
    for (int i = 0; i < pl->ncmds; i++) {
        int status = 0;
        waitpid(pids[i], &status, 0);
        if (i == pl->ncmds - 1)
            last_status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    return last_status;
}

/* Execute a full command list, honouring ;, &&, || connectors. */
void execute_cmdlist(struct cmdlist *cl) {
    int last_status = 0;
    for (int i = 0; i < cl->npl; i++) {
        struct pipeline *pl = &cl->pl[i];

        /* Decide whether to run this pipeline based on previous connector */
        if (i > 0) {
            int prev_conn = cl->pl[i-1].connector;
            if (prev_conn == CONN_AND && last_status != 0)
                continue;   /* skip: previous failed */
            if (prev_conn == CONN_OR  && last_status == 0)
                continue;   /* skip: previous succeeded */
            /* CONN_SEQ always runs */
        }

        last_status = execute_pipeline(pl);
    }
}
