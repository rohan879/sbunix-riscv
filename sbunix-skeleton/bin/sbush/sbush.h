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

struct command {
    char *argv[MAX_ARGS];
    int   argc;
};

struct pipeline {
    struct command cmds[MAX_PIPES];
    int  ncmds;
    int  background;
};

/* vars.c */
void  env_init(char **envp);
char *var_get(const char *name);
void  var_set(const char *name, const char *value);
void  var_export(const char *name);
char **build_environ(void);

/* parse.c */
int parse_line(char *line, struct pipeline *pl);

/* path.c */
char *resolve_path(const char *cmd);

/* builtin.c */
int is_builtin(const char *cmd);
int run_builtin(struct command *cmd);

/* exec.c */
void execute_pipeline(struct pipeline *pl);

/* script.c */
int run_script(const char *path);

/* sbush.c */
void execute_line(char *line);
