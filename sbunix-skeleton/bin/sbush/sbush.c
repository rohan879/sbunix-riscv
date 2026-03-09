#include "sbush.h"
#include <stdlib.h>
#include <fcntl.h>

/* Foreground child PID (0 when shell is at idle prompt) */
volatile sig_atomic_t fg_pid = 0;

/* Flag set by SIGCHLD handler; main loop checks and prints notifications */
volatile sig_atomic_t g_sigchld_flag = 0;

static int exec_depth = 0;
static struct cmdlist cl_stack[MAX_EXEC_DEPTH];

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

/* Scan line for unquoted << and enter heredoc mode. Returns 1 if found. */
static int check_heredoc(char *line) {
    char *p = line;
    int in_sq = 0, in_dq = 0;
    while (*p) {
        if (*p == '\'' && !in_dq) { in_sq = !in_sq; p++; continue; }
        if (*p == '"'  && !in_sq) { in_dq = !in_dq; p++; continue; }
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
 * Signal handlers
 * ---------------------------------------------------------------------- */

/* SIGINT handler: forward Ctrl-C to the foreground child if one exists */
static void sigint_handler(int sig) {
    (void)sig;
    pid_t p = fg_pid;
    if (p > 0)
        kill(p, SIGINT);
    /* If no foreground child, do nothing — read_line handles ^C via 0x03 */
}

/* SIGTSTP handler: forward Ctrl-Z to the foreground child if one exists */
static void sigtstp_handler(int sig) {
    (void)sig;
    pid_t p = fg_pid;
    if (p > 0)
        kill(p, SIGTSTP);
    /* Shell itself must NOT stop */
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

    /* script mode */
    if (argc >= 2)
        return run_script(argv[1]);

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
