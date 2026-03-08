#pragma once

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <syscall.h>
#include <stddef.h>
#include <stdlib.h>
#include <signal.h>

/* Wait macros not provided by syscall.h but needed for job control */
#ifndef WUNTRACED
#define WUNTRACED  2
#endif
#ifndef WIFSTOPPED
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#endif
#ifndef WSTOPSIG
#define WSTOPSIG(s)     (((s) >> 8) & 0xff)
#endif

#define MAX_LINE  1024
#define MAX_ARGS  64
#define MAX_PIPES 16
#define MAX_VARS  128

/* Connectors between pipelines in a command list */
#define CONN_NONE  0   /* last entry */
#define CONN_SEQ   1   /* ;  — always run next */
#define CONN_AND   2   /* && — run next only on success (exit 0) */
#define CONN_OR    3   /* || — run next only on failure (exit != 0) */

struct command {
    char *argv[MAX_ARGS];
    int   argc;
    char *redir_in;         /* < file */
    char *redir_out;        /* > file */
    char *redir_append;     /* >> file */
    char *redir_err;        /* 2> file */
    int   redir_err_to_out; /* 2>&1 */
};

struct pipeline {
    struct command cmds[MAX_PIPES];
    int  ncmds;
    int  background;  /* trailing & */
    int  connector;   /* CONN_* — how this pipeline connects to the next */
    char raw[MAX_LINE]; /* raw segment text for deferred expansion */
};

/* A command list: one or more pipelines joined by ;, &&, || */
#define MAX_CMDLIST 16

struct cmdlist {
    struct pipeline pl[MAX_CMDLIST];
    int             npl;
};

/* Job control */
#define MAX_JOBS 16

enum job_state { JOB_RUNNING, JOB_STOPPED, JOB_DONE };

struct job {
    int            id;       /* job number [1..N] */
    pid_t          pid;      /* lead process PID */
    enum job_state state;
    char           cmd[128]; /* command string for display */
};

/* jobs.c */
int         job_add(pid_t pid, const char *cmd);
void        job_remove(pid_t pid);
void        job_set_state(pid_t pid, enum job_state state);
struct job *job_find(int id);
struct job *job_find_pid(pid_t pid);
void        job_list(void);
void        job_reap(void);

/* is_name_char: check if char is valid in a variable/function name */
static inline int is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Recursion depth limit for execute_line */
#define MAX_EXEC_DEPTH 4

/* vars.c */
void  env_init(char **envp);
char *var_get(const char *name);
void  var_set(const char *name, const char *value);
void  var_unset(const char *name);
void  var_export(const char *name);
char **build_environ(void);

/* Positional parameters (vars.c) */
#define MAX_POSITIONAL 64
extern int positional_count;
char *positional_get(int idx);    /* 0=$0, 1=$1, etc. */
void  positional_set_args(int argc, char **argv);
void  positional_shift(int n);
void  positional_set_zero(const char *name);

/* parse.c */
int parse_line(char *line, struct cmdlist *cl);
int expand_and_parse_pipeline(char *seg, struct pipeline *pl);
int glob_match(const char *pat, const char *name);

/* path.c */
char *resolve_path(const char *cmd);

/* builtin.c */
int is_builtin(const char *cmd);
int run_builtin(struct command *cmd);

/* exec.c */
int  execute_pipeline(struct pipeline *pl);
void execute_cmdlist(struct cmdlist *cl);
extern int   last_exit_status;
extern pid_t last_bg_pid;

/* script.c */
int run_script(const char *path);

/* sbush.c */
void execute_line(char *line);

/* signal globals (sbush.c) */
extern volatile sig_atomic_t fg_pid;
extern volatile sig_atomic_t g_sigchld_flag;
