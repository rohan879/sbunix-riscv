#include "sbush.h"
#include <stdlib.h>

/* Foreground child PID (0 when shell is at idle prompt) */
volatile sig_atomic_t fg_pid = 0;

/* Flag set by SIGCHLD handler; main loop checks and prints notifications */
volatile sig_atomic_t g_sigchld_flag = 0;

void execute_line(char *line) {
    /* static: struct cmdlist is ~130 KB; placing it on the stack overflows
     * the 32 KB user stack.  The shell is single-threaded so static is safe. */
    static struct cmdlist cl;
    if (parse_line(line, &cl) > 0)
        execute_cmdlist(&cl);
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
    char line[MAX_LINE];
    while (1) {
        reap_zombies();
        char *ps1 = var_get("PS1");
        if (!ps1) ps1 = "sbush$ ";
        int r = read_line(line, sizeof(line), ps1);
        if (r == -1)
            break;       /* EOF */
        execute_line(line);
    }

    return 0;
}
