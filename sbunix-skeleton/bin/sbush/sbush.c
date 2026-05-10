#include "sbush.h"
#include <stdlib.h>
#include <fcntl.h>

/* Foreground process group (0 when shell is at idle prompt) */
volatile sig_atomic_t fg_pid = 0;

/* Flag set by SIGCHLD handler; main loop checks and prints notifications */
volatile sig_atomic_t g_sigchld_flag = 0;

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

static void execute_heredoc(void) {
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
            while (*p == ' ' || *p == '\t') p++;
            int di = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != ';' && di < 63)
                hd_delim[di++] = *p++;
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
        char *stripped = line;
        while (*stripped == ' ' || *stripped == '\t') stripped++;
        if (strcmp(stripped, hd_delim) == 0) {
            hd_active = 0;
            execute_heredoc();
        } else {
            int slen = strlen(line);
            if (hd_len + slen + 1 < CF_BODY_SIZE) {
                memcpy(hd_buf + hd_len, line, slen);
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

    /* Standalone VAR=value assignment (no command follows) */
    {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p && *p != '=' && is_name_char(*p)) {
            char *s = p;
            while (is_name_char(*s)) s++;
            if (*s == '=' && s > p) {
                /* Check that there's nothing after the value (simple assign) */
                char *vstart = s + 1;
                char *vend = vstart;
                int in_sq = 0, in_dq = 0;
                while (*vend) {
                    if (*vend == '\'' && !in_dq) in_sq = !in_sq;
                    else if (*vend == '"' && !in_sq) in_dq = !in_dq;
                    else if ((*vend == ' ' || *vend == '\t') && !in_sq && !in_dq) break;
                    vend++;
                }
                /* Skip trailing whitespace */
                char *trail = vend;
                while (*trail == ' ' || *trail == '\t') trail++;
                if (*trail == '\0' || *trail == '#') {
                    /* Pure assignment: NAME=VALUE */
                    char name[128];
                    int nlen = s - p;
                    if (nlen > 127) nlen = 127;
                    memcpy(name, p, nlen);
                    name[nlen] = '\0';
                    /* Extract raw value, strip outer quotes */
                    char val[MAX_LINE];
                    int vi = 0;
                    char *vp = vstart;
                    while (vp < vend && vi < MAX_LINE - 1) {
                        if ((*vp == '\'' || *vp == '"')) { vp++; continue; }
                        val[vi++] = *vp++;
                    }
                    val[vi] = '\0';
                    /* Expand $VAR, $1, $(cmd), etc. in the value */
                    expand_vars(val);
                    var_set(name, val);
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

/* -----------------------------------------------------------------------
 * Signal handlers
 * ---------------------------------------------------------------------- */

/* SIGINT handler: forward Ctrl-C to the foreground child if one exists */
static void sigint_handler(int sig) {
    (void)sig;
    pid_t p = fg_pid;
    if (p > 0)
        kill(-p, SIGINT);
    /* If no foreground child, do nothing — tty already echoed ^C\n, reprint prompt */
}

/* SIGQUIT handler: forward Ctrl-\ to the foreground child if one exists.
 * At the idle prompt the TTY echoes "^\n" and read() returns -EINTR,
 * so the shell's restart: label already reprints the prompt. */
static void sigquit_handler(int sig) {
    (void)sig;
    pid_t p = fg_pid;
    if (p > 0)
        kill(-p, SIGQUIT);
    /* If no foreground child, do nothing — tty already echoed ^\\n */
}

/* SIGTSTP handler: forward Ctrl-Z to the foreground child if one exists */
static void sigtstp_handler(int sig) {
    (void)sig;
    pid_t p = fg_pid;
    if (p > 0)
        kill(-p, SIGTSTP);
    /* Shell itself must NOT stop */
}

/* SIGCHLD handler: reap children and set flag for main-loop notification */
static void sigchld_handler(int sig) {
    (void)sig;
    /* BF: Defer waitpid() to normal shell flow so foreground waits own
     * their child status and stale job cleanup happens at command boundaries. */
    g_sigchld_flag = 1;
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

/* Print notifications for completed/stopped background jobs */
static void notify_jobs(void) {
    if (!g_sigchld_flag)
        return;
    g_sigchld_flag = 0;
    job_sync(1);
}

static void reap_zombies(void) {
    notify_jobs();
    job_sync(1);
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
    if (getcwd(cwd, sizeof(cwd))) {
        var_set("PWD", cwd);
        /* BF: Export PWD so child processes inherit the current directory.
         * POSIX requires PWD to be in the environment. */
        var_export("PWD");
    }

    /* -c mode: execute command string */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        execute_line(argv[2]);
        return last_exit_status;
    }

    /* script mode */
    if (argc >= 2) {
        positional_set_zero(argv[1]);
        if (argc > 2)
            positional_set_args(argc - 2, argv + 2);
        return run_script(argv[1]);
    }

    /* interactive REPL */
    static char line[MAX_LINE];
    while (1) {
        reap_zombies();
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
        execute_line(line);
    }

    return 0;
}
