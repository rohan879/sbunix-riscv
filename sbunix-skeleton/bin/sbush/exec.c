#include "sbush.h"
#include <stdlib.h>

void execute_pipeline(struct pipeline *pl) {
    if (pl->ncmds == 0)
        return;

    /* single builtin — run in-process */
    if (pl->ncmds == 1 && is_builtin(pl->cmds[0].argv[0])) {
        run_builtin(&pl->cmds[0]);
        return;
    }

    pid_t pids[MAX_PIPES];
    int prev_fd = -1;

    for (int i = 0; i < pl->ncmds; i++) {
        int pipefd[2] = {-1, -1};
        if (i < pl->ncmds - 1) {
            if (pipe(pipefd) < 0) {
                printf("sbush: pipe failed\n");
                return;
            }
        }

        pid_t pid = fork();
        if (pid < 0) {
            printf("sbush: fork failed\n");
            return;
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

            /* handle builtins in pipeline child */
            if (is_builtin(pl->cmds[i].argv[0])) {
                int ret = run_builtin(&pl->cmds[i]);
                exit(ret);
            }

            char *path = resolve_path(pl->cmds[i].argv[0]);
            if (!path) {
                printf("sbush: %s: command not found\n", pl->cmds[i].argv[0]);
                exit(127);
            }
            char **envp = build_environ();
            execve(path, pl->cmds[i].argv, envp);
            printf("sbush: %s: exec failed\n", pl->cmds[i].argv[0]);
            exit(126);
        }

        pids[i] = pid;
        if (prev_fd >= 0)
            close(prev_fd);
        if (pipefd[1] >= 0)
            close(pipefd[1]);
        prev_fd = pipefd[0];
    }

    if (!pl->background) {
        for (int i = 0; i < pl->ncmds; i++)
            waitpid(pids[i], NULL, 0);
    }
}
