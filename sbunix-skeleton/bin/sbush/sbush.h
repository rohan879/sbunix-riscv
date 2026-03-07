#pragma once

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <syscall.h>
#include <stddef.h>
#include <stdlib.h>

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
};

struct pipeline {
    struct command cmds[MAX_PIPES];
    int  ncmds;
    int  background;  /* trailing & */
    int  connector;   /* CONN_* — how this pipeline connects to the next */
};

/* A command list: one or more pipelines joined by ;, &&, || */
#define MAX_CMDLIST 16

struct cmdlist {
    struct pipeline pl[MAX_CMDLIST];
    int             npl;
};

/* vars.c */
void  env_init(char **envp);
char *var_get(const char *name);
void  var_set(const char *name, const char *value);
void  var_export(const char *name);
char **build_environ(void);

/* parse.c */
int parse_line(char *line, struct cmdlist *cl);

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
