#include "sbush.h"
#include <stdlib.h>
#include <fcntl.h>

int last_exit_status = 0;
pid_t last_bg_pid = 0;

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

            /* Apply file redirections */
            struct command *c = &pl->cmds[i];
            if (c->redir_in) {
                int fd = open(c->redir_in, O_RDONLY);
                if (fd < 0) {
                    dprintf(2, "sbush: %s: No such file or directory\n",
                            c->redir_in);
                    exit(1);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
            if (c->redir_out) {
                int fd = open(c->redir_out,
                              O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    dprintf(2, "sbush: %s: Cannot create file\n",
                            c->redir_out);
                    exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
            if (c->redir_append) {
                int fd = open(c->redir_append,
                              O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fd < 0) {
                    dprintf(2, "sbush: %s: Cannot open file\n",
                            c->redir_append);
                    exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
            if (c->redir_err) {
                int fd = open(c->redir_err,
                              O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    dprintf(2, "sbush: %s: Cannot create file\n",
                            c->redir_err);
                    exit(1);
                }
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
            if (c->redir_err_to_out) {
                dup2(STDOUT_FILENO, STDERR_FILENO);
            }

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
        last_bg_pid = pids[pl->ncmds - 1];
        /* Build command string for job display */
        char jobcmd[128] = "";
        int jlen = 0;
        for (int j = 0; j < pl->cmds[0].argc && jlen < 100; j++) {
            if (j > 0) jobcmd[jlen++] = ' ';
            const char *a = pl->cmds[0].argv[j];
            while (*a && jlen < 126)
                jobcmd[jlen++] = *a++;
        }
        jobcmd[jlen] = '\0';
        int jid = job_add(last_bg_pid, jobcmd);
        printf("[%d] %d\n", jid, (int)last_bg_pid);
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
    int status = 0;
    for (int i = 0; i < cl->npl; i++) {
        struct pipeline *pl = &cl->pl[i];

        /* Decide whether to run this pipeline based on previous connector */
        if (i > 0) {
            int prev_conn = cl->pl[i-1].connector;
            if (prev_conn == CONN_AND && status != 0)
                continue;   /* skip: previous failed */
            if (prev_conn == CONN_OR  && status == 0)
                continue;   /* skip: previous succeeded */
            /* CONN_SEQ always runs */
        }

        /* Expand variables and parse now (deferred from parse_line) */
        char buf[MAX_LINE];
        strncpy(buf, pl->raw, MAX_LINE - 1);
        buf[MAX_LINE - 1] = '\0';
        int saved_conn = pl->connector;
        if (!expand_and_parse_pipeline(buf, pl)) {
            pl->connector = saved_conn;
            continue;
        }
        pl->connector = saved_conn;

        status = execute_pipeline(pl);
        last_exit_status = status;
    }
}
