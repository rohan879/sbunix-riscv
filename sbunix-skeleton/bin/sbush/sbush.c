#include "sbush.h"
#include <stdlib.h>
#include <fcntl.h>

/* Foreground child PID (0 when shell is at idle prompt) */
volatile sig_atomic_t fg_pid = 0;

/* Flag set by SIGCHLD handler; main loop checks and prints notifications */
volatile sig_atomic_t g_sigchld_flag = 0;

/* 1 when running interactively (REPL), 0 in script/-c mode */
int shell_interactive = 0;

static int exec_depth = 0;
static struct cmdlist cl_stack[MAX_EXEC_DEPTH];

void exec_reset_depth(void) { exec_depth = 0; }

/* -----------------------------------------------------------------------
 * Here-document state
 * ---------------------------------------------------------------------- */
static int hd_active = 0;
static char hd_delim[64];
static char hd_cmd[MAX_LINE];
static char hd_buf[CF_BODY_SIZE];
static int hd_len = 0;
static int hd_quoted = 0;     /* 1 if delimiter was quoted — no expansion */
static int hd_strip_tabs = 0; /* 1 if <<- was used — strip leading tabs */

/* Simple variable expansion for heredoc body text (not full shell parsing).
 * Expands $NAME and ${NAME} references in-place. */
static void heredoc_expand_vars(char *buf, int *len) {
    static char tmp[CF_BODY_SIZE];
    char *src = buf;
    char *srcend = buf + *len;
    char *dst = tmp;
    char *dstend = tmp + CF_BODY_SIZE - 1;

    while (src < srcend && dst < dstend) {
        if (*src == '\\' && (src + 1) < srcend && *(src + 1) == '$') {
            /* Escaped $ — emit literal $ */
            src++;
            *dst++ = *src++;
        } else if (*src == '$') {
            src++;
            if (src < srcend && *src == '{') {
                /* ${NAME} */
                src++;
                char name[64];
                int ni = 0;
                while (src < srcend && *src != '}' && ni < 63)
                    name[ni++] = *src++;
                name[ni] = '\0';
                if (src < srcend && *src == '}') src++;
                char *val = var_get(name);
                if (val) {
                    while (*val && dst < dstend) *dst++ = *val++;
                }
            } else if (src < srcend && *src == '?') {
                src++;
                char tmp2[16];
                itoa(last_exit_status, tmp2, 10);
                for (char *v = tmp2; *v && dst < dstend; )
                    *dst++ = *v++;
            } else if (src < srcend && *src == '$') {
                src++;
                char tmp2[16];
                itoa((int)getpid(), tmp2, 10);
                for (char *v = tmp2; *v && dst < dstend; )
                    *dst++ = *v++;
            } else if (src < srcend && *src >= '0' && *src <= '9') {
                int idx = *src - '0'; src++;
                char *pv = positional_get(idx);
                if (pv) { while (*pv && dst < dstend) *dst++ = *pv++; }
            } else {
                /* $NAME */
                char name[64];
                int ni = 0;
                while (src < srcend && is_name_char(*src) && ni < 63)
                    name[ni++] = *src++;
                name[ni] = '\0';
                char *val = var_get(name);
                if (val) {
                    while (*val && dst < dstend) *dst++ = *val++;
                }
            }
        } else {
            *dst++ = *src++;
        }
    }
    *len = dst - tmp;
    memcpy(buf, tmp, *len);
}

static void execute_heredoc(void) {
    /* If delimiter was unquoted, expand variables in body */
    if (!hd_quoted)
        heredoc_expand_vars(hd_buf, &hd_len);

    int pfd[2];
    if (pipe(pfd) < 0) {
        dprintf(2, "sbush: heredoc pipe failed\n");
        return;
    }
    /* Write heredoc text in a child to avoid blocking on full pipe */
    pid_t wpid = fork();
    if (wpid == 0) {
        close(pfd[0]);
        write(pfd[1], hd_buf, hd_len);
        close(pfd[1]);
        _exit(0);
    }
    close(pfd[1]);
    /* Redirect stdin to pipe read end */
    int saved_stdin = dup(STDIN_FILENO);
    dup2(pfd[0], STDIN_FILENO);
    close(pfd[0]);
    /* Execute the command */
    execute_line(hd_cmd);
    /* Restore stdin */
    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);
    /* Reap writer child */
    waitpid(wpid, NULL, 0);
}

/* Scan line for unquoted << and enter heredoc mode. Returns 1 if found.
 * Skips over $((...)) and $(...) so that << inside arithmetic or command
 * substitution is not mistaken for a heredoc operator. */
static int check_heredoc(char *line) {
    char *p = line;
    int in_sq = 0, in_dq = 0;
    while (*p) {
        if (*p == '\'' && !in_dq) { in_sq = !in_sq; p++; continue; }
        if (*p == '"'  && !in_sq) { in_dq = !in_dq; p++; continue; }
        if (!in_sq) {
            /* Skip over $((…)) arithmetic expansion */
            if (*p == '$' && p[1] == '(' && p[2] == '(') {
                p += 3;
                int depth = 1;
                while (*p && depth > 0) {
                    if (p[0] == '(' && p[1] == '(') { depth++; p += 2; }
                    else if (p[0] == ')' && p[1] == ')') { depth--; p += 2; }
                    else p++;
                }
                continue;
            }
            /* Skip over $(…) command substitution */
            if (*p == '$' && p[1] == '(') {
                p += 2;
                int depth = 1;
                while (*p && depth > 0) {
                    if (*p == '(') depth++;
                    else if (*p == ')') depth--;
                    if (depth > 0) p++;
                }
                if (*p == ')') p++;
                continue;
            }
            /* Skip over `…` backtick command substitution */
            if (*p == '`') {
                p++;
                while (*p && *p != '`') p++;
                if (*p == '`') p++;
                continue;
            }
        }
        if (!in_sq && !in_dq && *p == '<' && *(p+1) == '<' && *(p+2) != '<') {
            char *hd_start = p;
            p += 2;
            /* Check for <<- (tab-stripping) */
            hd_strip_tabs = 0;
            if (*p == '-') {
                hd_strip_tabs = 1;
                p++;
            }
            while (*p == ' ' || *p == '\t') p++;
            /* Check for quoted delimiter */
            hd_quoted = 0;
            int di = 0;
            if (*p == '\'' || *p == '"') {
                /* Quoted delimiter — no variable expansion in body */
                char quote = *p++;
                hd_quoted = 1;
                while (*p && *p != quote && di < 63)
                    hd_delim[di++] = *p++;
                if (*p == quote) p++;
            } else {
                while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != ';' && di < 63)
                    hd_delim[di++] = *p++;
            }
            hd_delim[di] = '\0';
            if (di == 0) return 0;
            *hd_start = '\0';
            strncpy(hd_cmd, line, MAX_LINE - 1);
            hd_cmd[MAX_LINE - 1] = '\0';
            hd_active = 1;
            hd_len = 0;
            return 1;
        }
        p++;
    }
    return 0;
}

int heredoc_active(void) { return hd_active; }

void execute_line(char *line) {
    /* Here-document accumulation */
    if (hd_active) {
        char *check = line;
        /* For <<-, strip leading tabs for delimiter comparison */
        if (hd_strip_tabs) {
            while (*check == '\t') check++;
        }
        if (strcmp(check, hd_delim) == 0) {
            hd_active = 0;
            execute_heredoc();
        } else {
            char *body_line = line;
            /* For <<-, strip leading tabs from body lines */
            if (hd_strip_tabs) {
                while (*body_line == '\t') body_line++;
            }
            int slen = strlen(body_line);
            if (hd_len + slen + 1 < CF_BODY_SIZE) {
                memcpy(hd_buf + hd_len, body_line, slen);
                hd_len += slen;
                hd_buf[hd_len++] = '\n';
            }
        }
        return;
    }

    /* Function definition check */
    if (func_handle_line(line))
        return;

    if (cf_handle_line(line))
        return;

    /* Check for heredoc before parsing */
    if (check_heredoc(line))
        return;

    /* Check for standalone VAR=value assignments.
     * If ALL whitespace-separated tokens are NAME=VALUE, set them and return.
     * Skip leading whitespace, then examine each token. */
    {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p && *p != '#') {
            /* Check if this line is purely assignments */
            char tmp[MAX_LINE];
            strncpy(tmp, p, MAX_LINE - 1);
            tmp[MAX_LINE - 1] = '\0';
            char *t = tmp;
            int all_assign = 1;
            int any_token = 0;
            while (*t) {
                while (*t == ' ' || *t == '\t') t++;
                if (!*t) break;
                /* Extract token start */
                char *tok = t;
                while (*t && *t != ' ' && *t != '\t') t++;
                if (*t) *t++ = '\0';
                any_token = 1;
                /* Check if tok matches NAME=VALUE */
                char *eq = strchr(tok, '=');
                if (!eq || eq == tok) { all_assign = 0; break; }
                /* Validate NAME: [A-Za-z_][A-Za-z0-9_]* */
                char *nc = tok;
                if (!((*nc >= 'A' && *nc <= 'Z') || (*nc >= 'a' && *nc <= 'z') || *nc == '_')) {
                    all_assign = 0; break;
                }
                nc++;
                while (nc < eq) {
                    if (!((*nc >= 'A' && *nc <= 'Z') || (*nc >= 'a' && *nc <= 'z') ||
                          (*nc >= '0' && *nc <= '9') || *nc == '_')) {
                        all_assign = 0; break;
                    }
                    nc++;
                }
                if (!all_assign) break;
            }
            if (any_token && all_assign) {
                /* Re-parse the original line and set variables */
                char tmp2[MAX_LINE];
                strncpy(tmp2, p, MAX_LINE - 1);
                tmp2[MAX_LINE - 1] = '\0';
                char *t2 = tmp2;
                while (*t2) {
                    while (*t2 == ' ' || *t2 == '\t') t2++;
                    if (!*t2) break;
                    char *tok2 = t2;
                    while (*t2 && *t2 != ' ' && *t2 != '\t') t2++;
                    if (*t2) *t2++ = '\0';
                    char *eq2 = strchr(tok2, '=');
                    if (eq2) {
                        *eq2 = '\0';
                        var_set(tok2, eq2 + 1);
                    }
                }
                return;
            }
        }
    }

    /* Check for VAR=value prefix assignments before a command.
     * Pattern: one or more NAME=VALUE tokens followed by a non-assignment token.
     * The assignments are set temporarily, scoped only for this command. */
    {
        char *p2 = line;
        while (*p2 == ' ' || *p2 == '\t') p2++;
        if (*p2 && *p2 != '#') {
            /* Count leading assignment tokens */
            static char assign_tmp[MAX_LINE];
            strncpy(assign_tmp, p2, MAX_LINE - 1);
            assign_tmp[MAX_LINE - 1] = '\0';
            char *at = assign_tmp;
            int nassign = 0;
            static char anames[MAX_ARGS][64];
            static char avalues[MAX_ARGS][256];
            static char saved_vals[MAX_ARGS][256];
            static int  saved_existed[MAX_ARGS];

            while (*at) {
                while (*at == ' ' || *at == '\t') at++;
                if (!*at) break;
                char *tok_start = at;
                while (*at && *at != ' ' && *at != '\t') at++;
                char saved_ch = *at;
                if (*at) *at = '\0';
                /* Check if tok_start is NAME=VALUE */
                char *eq = strchr(tok_start, '=');
                if (!eq || eq == tok_start) {
                    *at = saved_ch;
                    break; /* not an assignment; this is the command */
                }
                /* Validate NAME */
                char *nc2 = tok_start;
                int valid_name = ((*nc2 >= 'A' && *nc2 <= 'Z') ||
                                  (*nc2 >= 'a' && *nc2 <= 'z') || *nc2 == '_');
                nc2++;
                while (nc2 < eq && valid_name) {
                    if (!((*nc2 >= 'A' && *nc2 <= 'Z') || (*nc2 >= 'a' && *nc2 <= 'z') ||
                          (*nc2 >= '0' && *nc2 <= '9') || *nc2 == '_'))
                        valid_name = 0;
                    nc2++;
                }
                if (!valid_name) {
                    *at = saved_ch;
                    break;
                }
                /* Record this assignment */
                if (nassign < MAX_ARGS) {
                    int nlen = eq - tok_start;
                    if (nlen > 63) nlen = 63;
                    memcpy(anames[nassign], tok_start, nlen);
                    anames[nassign][nlen] = '\0';
                    strncpy(avalues[nassign], eq + 1, 255);
                    avalues[nassign][255] = '\0';
                    nassign++;
                }
                if (saved_ch) {
                    *at = saved_ch;
                    at++;
                }
            }
            /* If we found assignments AND there is a remaining command */
            while (*at == ' ' || *at == '\t') at++;
            if (nassign > 0 && *at) {
                /* Find the command portion in the original line */
                char *orig_cmd = p2;
                int skip_count = nassign;
                while (skip_count > 0 && *orig_cmd) {
                    while (*orig_cmd == ' ' || *orig_cmd == '\t') orig_cmd++;
                    while (*orig_cmd && *orig_cmd != ' ' && *orig_cmd != '\t') orig_cmd++;
                    skip_count--;
                }
                while (*orig_cmd == ' ' || *orig_cmd == '\t') orig_cmd++;

                if (*orig_cmd) {
                    /* Save old values, set new ones */
                    for (int ai = 0; ai < nassign; ai++) {
                        char *old = var_get(anames[ai]);
                        if (old) {
                            saved_existed[ai] = 1;
                            strncpy(saved_vals[ai], old, 255);
                            saved_vals[ai][255] = '\0';
                        } else {
                            saved_existed[ai] = 0;
                        }
                        var_set(anames[ai], avalues[ai]);
                        var_export(anames[ai]);
                    }
                    /* Execute the command portion */
                    static char cmd_portion[MAX_LINE];
                    strncpy(cmd_portion, orig_cmd, MAX_LINE - 1);
                    cmd_portion[MAX_LINE - 1] = '\0';

                    if (exec_depth >= MAX_EXEC_DEPTH) {
                        dprintf(2, "sbush: execution recursion limit reached\n");
                    } else {
                        struct cmdlist *cl2 = &cl_stack[exec_depth++];
                        if (parse_line(cmd_portion, cl2) > 0)
                            execute_cmdlist(cl2);
                        exec_depth--;
                    }

                    /* Restore old values */
                    for (int ai = 0; ai < nassign; ai++) {
                        if (saved_existed[ai])
                            var_set(anames[ai], saved_vals[ai]);
                        else
                            var_unset(anames[ai]);
                    }
                    return;
                }
            }
        }
    }

    if (exec_depth >= MAX_EXEC_DEPTH) {
        dprintf(2, "sbush: execution recursion limit reached\n");
        return;
    }
    struct cmdlist *cl = &cl_stack[exec_depth++];
    if (parse_line(line, cl) > 0)
        execute_cmdlist(cl);
    exec_depth--;
}

/* -----------------------------------------------------------------------
 * Shell trap table — stores commands to run on signal delivery
 * ---------------------------------------------------------------------- */
#define MAX_TRAPS 32
static char trap_cmds[MAX_TRAPS][256];
static int  trap_set_flag[MAX_TRAPS];

void trap_set(int sig, const char *action) {
    if (sig < 0 || sig >= MAX_TRAPS) return;
    if (action[0] == '\0' || strcmp(action, "-") == 0) {
        /* Reset trap */
        trap_set_flag[sig] = 0;
        trap_cmds[sig][0] = '\0';
    } else {
        strncpy(trap_cmds[sig], action, 255);
        trap_cmds[sig][255] = '\0';
        trap_set_flag[sig] = 1;
    }
}

void trap_run(int sig) {
    if (sig >= 0 && sig < MAX_TRAPS && trap_set_flag[sig]) {
        static char trap_line[256];
        strncpy(trap_line, trap_cmds[sig], 255);
        trap_line[255] = '\0';
        execute_line(trap_line);
    }
}

static const char *sig_name(int sig) {
    switch (sig) {
        case 0:       return "EXIT";
        case SIGHUP:  return "HUP";
        case SIGINT:  return "INT";
        case SIGQUIT: return "QUIT";
        case SIGILL:  return "ILL";
        case SIGTRAP: return "TRAP";
        case SIGABRT: return "ABRT";
        case SIGBUS:  return "BUS";
        case SIGFPE:  return "FPE";
        case SIGKILL: return "KILL";
        case SIGUSR1: return "USR1";
        case SIGSEGV: return "SEGV";
        case SIGUSR2: return "USR2";
        case SIGPIPE: return "PIPE";
        case SIGALRM: return "ALRM";
        case SIGTERM: return "TERM";
        case SIGCHLD: return "CHLD";
        case SIGCONT: return "CONT";
        case SIGSTOP: return "STOP";
        case SIGTSTP: return "TSTP";
        default:      return NULL;
    }
}

void trap_list(void) {
    for (int i = 0; i < MAX_TRAPS; i++) {
        if (!trap_set_flag[i]) continue;
        const char *name = sig_name(i);
        if (name)
            printf("trap -- '%s' %s\n", trap_cmds[i], name);
        else
            printf("trap -- '%s' %d\n", trap_cmds[i], i);
    }
}

/* -----------------------------------------------------------------------
 * Signal handlers
 * ---------------------------------------------------------------------- */

/* Pending trap flags — set by signal handlers, run from main loop.
 * This avoids calling async-signal-unsafe execute_line() from handlers. */
static volatile sig_atomic_t trap_pending[MAX_TRAPS];

/* SIGINT handler: forward Ctrl-C to the foreground child if one exists */
static void sigint_handler(int sig) {
    (void)sig;
    pid_t p = fg_pid;
    if (p > 0)
        kill(p, SIGINT);
    trap_pending[SIGINT] = 1;
}

/* SIGQUIT handler: forward Ctrl-\ to the foreground child if one exists */
static void sigquit_handler(int sig) {
    (void)sig;
    pid_t p = fg_pid;
    if (p > 0)
        kill(p, SIGQUIT);
    trap_pending[SIGQUIT] = 1;
}

/* SIGTSTP handler: forward Ctrl-Z to the foreground child if one exists */
static void sigtstp_handler(int sig) {
    (void)sig;
    pid_t p = fg_pid;
    if (p > 0)
        kill(p, SIGTSTP);
    trap_pending[SIGTSTP] = 1;
}

/* SIGCHLD handler: reap children and set flag for main-loop notification */
static void sigchld_handler(int sig) {
    (void)sig;
    int saved_status;
    pid_t p;
    while ((p = waitpid(-1, &saved_status, WNOHANG | WUNTRACED)) > 0) {
        if (WIFSTOPPED(saved_status)) {
            job_set_state(p, JOB_STOPPED);
        } else {
            job_set_state(p, JOB_DONE);
        }
        g_sigchld_flag = 1;
    }
}

/* Install all three signal handlers at shell startup */
static void install_signal_handlers(void) {
    struct sigaction sa;

    /* SIGINT */
    sa.sa_handler = sigint_handler;
    sa.sa_mask = 0;
    sa.sa_flags = SA_RESTART;
    sa.sa_restorer = NULL;
    sigaction(SIGINT, &sa, NULL);

    /* SIGQUIT */
    sa.sa_handler = sigquit_handler;
    sa.sa_mask = 0;
    sa.sa_flags = SA_RESTART;
    sa.sa_restorer = NULL;
    sigaction(SIGQUIT, &sa, NULL);

    /* SIGTSTP */
    sa.sa_handler = sigtstp_handler;
    sa.sa_mask = 0;
    sa.sa_flags = SA_RESTART;
    sa.sa_restorer = NULL;
    sigaction(SIGTSTP, &sa, NULL);

    /* SIGCHLD */
    sa.sa_handler = sigchld_handler;
    sa.sa_mask = 0;
    sa.sa_flags = SA_RESTART;
    sa.sa_restorer = NULL;
    sigaction(SIGCHLD, &sa, NULL);
}

/* Read a line from stdin.  The kernel tty layer (cooked mode) handles echo,
 * backspace, ^C (SIGINT), ^Z (SIGTSTP), and ^D (EOF).  We just read the
 * completed line.
 * Returns the number of characters in buf (excluding NUL), or -1 on EOF. */
static int read_line(char *buf, int max, const char *ps1) {
restart:
    write(STDOUT_FILENO, ps1, strlen(ps1));
    ssize_t n = read(STDIN_FILENO, buf, max - 1);
    if (n == 0)
        return -1;   /* EOF (^D on empty line) */
    if (n < 0) {
        /* EINTR from ^C — tty already echoed ^C\n, reprint prompt */
        goto restart;
    }
    /* Strip trailing newline */
    if (n > 0 && buf[n - 1] == '\n')
        n--;
    buf[n] = '\0';
    return (int)n;
}

/* Process any deferred trap commands from signal handlers.
 * Called from the main loop, not from signal context. */
static void process_pending_traps(void) {
    for (int i = 0; i < MAX_TRAPS; i++) {
        if (trap_pending[i]) {
            trap_pending[i] = 0;
            trap_run(i);
        }
    }
}

/* Print notifications for completed/stopped background jobs */
static void notify_jobs(void) {
    if (!g_sigchld_flag)
        return;
    g_sigchld_flag = 0;
    job_reap();
}

static void reap_zombies(void) {
    notify_jobs();
    /* Reap any remaining zombies and update job table */
    int status;
    pid_t p;
    while ((p = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        struct job *j = job_find_pid(p);
        if (j) {
            if (WIFSTOPPED(status)) {
                j->state = JOB_STOPPED;
            } else {
                printf("[%d] Done\t%s\n", j->id, j->cmd);
                j->pid = 0;
                j->id = 0;
            }
        }
    }
}

int main(int argc, char *argv[], char *envp[]) {
    env_init(envp);
    positional_set_zero(argv[0]);

    /* Install signal handlers before entering REPL */
    install_signal_handlers();

    /* set defaults */
    if (!var_get("PATH"))
        var_set("PATH", "/bin");
    if (!var_get("PS1"))
        var_set("PS1", "sbush$ ");
    var_export("PATH");

    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)))
        var_set("PWD", cwd);

    /* -c mode: execute command string */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        execute_line(argv[2]);
        trap_run(0);
        return last_exit_status;
    }

    /* script mode */
    if (argc >= 2) {
        positional_set_zero(argv[1]);
        if (argc > 2)
            positional_set_args(argc - 2, argv + 2);
        int ret = run_script(argv[1]);
        trap_run(0);
        return ret;
    }

    /* interactive REPL */
    shell_interactive = 1;
    static char line[MAX_LINE];
    while (1) {
        reap_zombies();
        process_pending_traps();
        char *ps1;
        if (cf_active() || heredoc_active() || func_def_active())
            ps1 = "> ";
        else {
            ps1 = var_get("PS1");
            if (!ps1) ps1 = "sbush$ ";
        }
        int r = read_line(line, sizeof(line), ps1);
        if (r == -1)
            break;       /* EOF */
        /* Backslash-newline continuation: if line ends with '\', read more */
        while (r > 0 && line[r - 1] == '\\') {
            line[r - 1] = '\0';  /* strip trailing backslash */
            r--;
            int space = sizeof(line) - r;
            if (space <= 1) break;
            int r2 = read_line(line + r, space, "> ");
            if (r2 == -1) break;
            r += r2;
        }
        execute_line(line);
    }

    /* Run EXIT trap before exiting */
    trap_run(0);
    return 0;
}
