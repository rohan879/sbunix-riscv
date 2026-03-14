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

    /* single builtin — run in-process (with save/restore for redirections) */
    if (pl->ncmds == 1 && !pl->background && is_builtin(pl->cmds[0].argv[0])) {
        struct command *c = &pl->cmds[0];
        int has_redir = c->redir_in || c->redir_out ||
                        c->redir_append || c->redir_err ||
                        c->redir_err_to_out;
        int is_exec = (strcmp(c->argv[0], "exec") == 0);

        if (!has_redir)
            return run_builtin(c);

        /* Save file descriptors that we're about to redirect.
         * For 'exec' builtin, redirections are permanent (no restore). */
        int saved_in = -1, saved_out = -1, saved_err = -1;
        if (!is_exec) {
            if (c->redir_in)
                saved_in = dup(STDIN_FILENO);
            if (c->redir_out || c->redir_append)
                saved_out = dup(STDOUT_FILENO);
            if (c->redir_err || c->redir_err_to_out)
                saved_err = dup(STDERR_FILENO);
        }

        /* Apply redirections */
        int redir_ok = 1;
        if (c->redir_in) {
            int fd = open(c->redir_in, O_RDONLY);
            if (fd < 0) {
                dprintf(2, "sbush: %s: No such file or directory\n", c->redir_in);
                redir_ok = 0;
            } else {
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
        }
        if (redir_ok && c->redir_out) {
            int fd = open(c->redir_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                dprintf(2, "sbush: %s: Cannot create file\n", c->redir_out);
                redir_ok = 0;
            } else {
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
        }
        if (redir_ok && c->redir_append) {
            int fd = open(c->redir_append, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd < 0) {
                dprintf(2, "sbush: %s: Cannot open file\n", c->redir_append);
                redir_ok = 0;
            } else {
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
        }
        if (redir_ok && c->redir_err) {
            int fd = open(c->redir_err, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                dprintf(2, "sbush: %s: Cannot create file\n", c->redir_err);
                redir_ok = 0;
            } else {
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
        }
        if (redir_ok && c->redir_err_to_out)
            dup2(STDOUT_FILENO, STDERR_FILENO);

        int ret = redir_ok ? run_builtin(c) : 1;

        /* Restore saved file descriptors (non-exec builtins only) */
        if (saved_in >= 0) {
            dup2(saved_in, STDIN_FILENO);
            close(saved_in);
        }
        if (saved_out >= 0) {
            dup2(saved_out, STDOUT_FILENO);
            close(saved_out);
        }
        if (saved_err >= 0) {
            dup2(saved_err, STDERR_FILENO);
            close(saved_err);
        }
        return ret;
    }

    /* single shell function — run in-process (unless it has redirections) */
    if (pl->ncmds == 1 && !pl->background && func_find(pl->cmds[0].argv[0]) &&
        !pl->cmds[0].redir_in && !pl->cmds[0].redir_out &&
        !pl->cmds[0].redir_append && !pl->cmds[0].redir_err) {
        return func_call(pl->cmds[0].argv[0],
                         pl->cmds[0].argc, pl->cmds[0].argv);
    }

    pid_t pids[MAX_PIPES];
    int prev_fd = -1;
    pid_t pgid = 0;  /* process group for this pipeline */

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
            /* child: join (or create) the pipeline's process group */
            setpgid(0, pgid);  /* pgid==0 for first child creates new group */

            /* Restore default signal dispositions in children */
            struct sigaction sa;
            sa.sa_handler = SIG_DFL;
            sa.sa_mask = 0;
            sa.sa_flags = 0;
            sa.sa_restorer = NULL;
            sigaction(SIGINT, &sa, NULL);
            sigaction(SIGTSTP, &sa, NULL);
            sigaction(SIGCHLD, &sa, NULL);

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

        /* parent: set the child into the pipeline process group (race-free) */
        if (i == 0)
            pgid = pid;  /* first child's PID becomes the group ID */
        setpgid(pid, pgid);

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

    /* Foreground pipeline: give terminal to the pipeline process group */
    tcsetpgrp(STDIN_FILENO, pgid);

    /* Track foreground PID for signal forwarding */
    fg_pid = pgid;

    /* wait for all processes; return exit status of last */
    int last_status = 0;
    for (int i = 0; i < pl->ncmds; i++) {
        int status = 0;
        waitpid(pids[i], &status, WUNTRACED);
        if (WIFSTOPPED(status)) {
            /* Child was stopped (e.g. by SIGTSTP / Ctrl-Z).
             * Add it to the job table as stopped. */
            char jobcmd[128] = "";
            int jlen = 0;
            for (int j = 0; j < pl->cmds[i].argc && jlen < 100; j++) {
                if (j > 0) jobcmd[jlen++] = ' ';
                const char *a = pl->cmds[i].argv[j];
                while (*a && jlen < 126)
                    jobcmd[jlen++] = *a++;
            }
            jobcmd[jlen] = '\0';
            /* Check if already in job table (e.g. from SIGCHLD handler) */
            struct job *existing = job_find_pid(pids[i]);
            if (existing) {
                existing->state = JOB_STOPPED;
            } else {
                job_add(pids[i], jobcmd);
                job_set_state(pids[i], JOB_STOPPED);
            }
            /* Find the job to print notification */
            struct job *sj = job_find_pid(pids[i]);
            if (sj)
                printf("[%d] Stopped\t%s\n", sj->id, sj->cmd);
            if (i == pl->ncmds - 1)
                last_status = 128 + WSTOPSIG(status);
        } else if (i == pl->ncmds - 1) {
            last_status = WIFEXITED(status) ? WEXITSTATUS(status) : (128 + WTERMSIG(status));
        }
    }

    /* Reclaim terminal for the shell */
    fg_pid = 0;
    tcsetpgrp(STDIN_FILENO, getpgrp());

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

        /* Check for ! pipeline negation prefix */
        int negate = 0;
        {
            char *rp = pl->raw;
            while (*rp == ' ' || *rp == '\t') rp++;
            if (*rp == '!' && (rp[1] == ' ' || rp[1] == '\t' || rp[1] == '\0')) {
                negate = 1;
                /* Remove the ! prefix from raw text */
                rp++;
                while (*rp == ' ' || *rp == '\t') rp++;
                memmove(pl->raw, rp, strlen(rp) + 1);
            }
        }

        /* Expand variables and parse now (deferred from parse_line).
         * Use pl->raw directly — it's not needed after expansion. */
        int saved_conn = pl->connector;
        if (!expand_and_parse_pipeline(pl->raw, pl)) {
            pl->connector = saved_conn;
            continue;
        }
        pl->connector = saved_conn;

        status = execute_pipeline(pl);
        if (negate)
            status = (status == 0) ? 1 : 0;
        last_exit_status = status;
    }
}
