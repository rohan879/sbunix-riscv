# Shell POSIX Features Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add missing POSIX shell features to sbush: `$?`/`$!`/`$$` expansion, single quotes, backslash escapes, file redirections, and job control builtins.

**Architecture:** All changes in `bin/sbush/` source files. The shell already has pipes, `&&`/`||`/`;`, background `&`, double-quote support, globbing, and environment variables. We're adding on top of the existing parsing and execution infrastructure.

**Tech Stack:** C (freestanding userspace), compiled with `riscv64-unknown-elf-gcc`. Shell source at `bin/sbush/` with files: `sbush.h`, `sbush.c`, `parse.c`, `exec.c`, `builtin.c`, `vars.c`, `path.c`, `script.c`.

**Key architecture facts:**
- `parse.c:expand_vars()` handles `$NAME` expansion at line 8-36
- `parse.c:tokenize()` handles double-quote parsing at lines 50-56
- `exec.c:execute_pipeline()` forks and execs at lines 6-85
- `exec.c:execute_cmdlist()` runs pipelines with connector logic at lines 88-105
- `sbush.h:struct command` holds `argv[MAX_ARGS]` and `argc`
- `sbush.h:struct pipeline` holds `cmds[MAX_PIPES]`, `ncmds`, `background`, `connector`

---

## Task 1: Add `$?` exit status tracking

**Files:**
- Modify: `bin/sbush/sbush.h` (add extern declaration)
- Modify: `bin/sbush/exec.c` (store last exit status globally)
- Modify: `bin/sbush/parse.c:expand_vars()` (expand `$?`)

The shell already computes `last_status` in `execute_cmdlist()` but it's a local variable. We need to make it global.

**Step 1: Add global in `exec.c`**

Add at top of `exec.c` after includes:

```c
int last_exit_status = 0;
```

In `execute_cmdlist()`, change the local `last_status` to update the global. Replace line 89:

```c
// was: int last_status = 0;
```

And at line 103, after `last_status = execute_pipeline(pl);`, add:

```c
last_exit_status = last_status;
```

Actually, the simplest approach: rename the local and assign to the global at the end. Replace the full function:

```c
void execute_cmdlist(struct cmdlist *cl) {
    int status = 0;
    for (int i = 0; i < cl->npl; i++) {
        struct pipeline *pl = &cl->pl[i];
        if (i > 0) {
            int prev_conn = cl->pl[i-1].connector;
            if (prev_conn == CONN_AND && status != 0)
                continue;
            if (prev_conn == CONN_OR  && status == 0)
                continue;
        }
        status = execute_pipeline(pl);
    }
    last_exit_status = status;
}
```

**Step 2: Declare extern in `sbush.h`**

Add after the `execute_cmdlist` declaration:

```c
extern int last_exit_status;
```

**Step 3: Handle `$?` in `expand_vars()` in `parse.c`**

In `expand_vars()`, after the `if (*src == '$')` check and `src++`, add special variable handling before the general name loop:

```c
if (*src == '$') {
    src++;
    /* Special variables */
    if (*src == '?') {
        src++;
        char tmp[16];
        itoa(last_exit_status, tmp, 10);
        char *v = tmp;
        while (*v && dst < end) *dst++ = *v++;
        continue;  /* use continue since this is in a while loop */
    }
    /* ... existing name parsing code ... */
}
```

Wait — the expand_vars function uses a while loop, not a for loop with continue. Let me restructure. The cleanest approach is to add the special variable checks right after `src++` inside the `if (*src == '$')` block, before the name-scanning loop:

In `parse.c`, replace the `expand_vars` function entirely:

```c
static void expand_vars(char *line) {
    char *src = line;
    char *dst = expand_buf;
    char *end = expand_buf + MAX_LINE - 1;

    while (*src && dst < end) {
        if (*src == '$') {
            src++;
            if (*src == '?') {
                /* $? — last exit status */
                src++;
                char tmp[16];
                itoa(last_exit_status, tmp, 10);
                for (char *v = tmp; *v && dst < end; )
                    *dst++ = *v++;
            } else if (*src == '!') {
                /* $! — last background PID */
                src++;
                extern pid_t last_bg_pid;
                char tmp[16];
                itoa((int)last_bg_pid, tmp, 10);
                for (char *v = tmp; *v && dst < end; )
                    *dst++ = *v++;
            } else if (*src == '$') {
                /* $$ — shell PID */
                src++;
                char tmp[16];
                itoa((int)getpid(), tmp, 10);
                for (char *v = tmp; *v && dst < end; )
                    *dst++ = *v++;
            } else {
                /* $NAME — regular variable */
                char name[64];
                int i = 0;
                while (*src && ((*src >= 'a' && *src <= 'z') ||
                                (*src >= 'A' && *src <= 'Z') ||
                                (*src >= '0' && *src <= '9') ||
                                *src == '_') && i < 63) {
                    name[i++] = *src++;
                }
                name[i] = '\0';
                char *val = var_get(name);
                if (val) {
                    while (*val && dst < end)
                        *dst++ = *val++;
                }
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    strcpy(line, expand_buf);
}
```

**Step 4: Build and verify**

Run: `make clean && make`
Expected: Compiles clean.

**Step 5: Test in QEMU**

```
make qemu
# At sbush$ prompt:
echo $?        # Should print "0"
ls /nonexist
echo $?        # Should print non-zero
```

**Step 6: Commit**

```bash
git add bin/sbush/sbush.h bin/sbush/exec.c bin/sbush/parse.c
git commit -m "ENH: Added $? exit status tracking to sbush"
```

---

## Task 2: Add `$!` background PID tracking

**Files:**
- Modify: `bin/sbush/exec.c` (store last background PID)
- Modify: `bin/sbush/sbush.h` (extern declaration)

**Step 1: Add global in `exec.c`**

Add at top of `exec.c`:

```c
pid_t last_bg_pid = 0;
```

In `execute_pipeline()`, when a background pipeline is launched, store the last child PID. After line 72 (`return 0;` in the background case), but before the return, add:

```c
if (pl->background) {
    last_bg_pid = pids[pl->ncmds - 1];
    if (prev_fd >= 0) close(prev_fd);
    return 0;
}
```

**Step 2: Declare in `sbush.h`**

```c
extern pid_t last_bg_pid;
```

**Step 3: Build, verify, commit**

The `$!` expansion was already added in Task 1's `expand_vars`. Just verify it works.

```bash
make clean && make
git add bin/sbush/exec.c bin/sbush/sbush.h
git commit -m "ENH: Added $! background PID tracking to sbush"
```

---

## Task 3: Single-quote support

**Files:**
- Modify: `bin/sbush/parse.c:tokenize()` (lines 38-67)
- Modify: `bin/sbush/parse.c:expand_vars()` — single-quoted regions should NOT expand variables

Single quotes preserve literal content — no variable expansion, no special character handling.

**Step 1: Add single-quote handling to `tokenize()`**

In the tokenize function, after the double-quote case (line 50-56), add a single-quote case:

```c
if (*p == '\'') {
    p++;
    cmd->argv[cmd->argc++] = p;
    while (*p && *p != '\'')
        p++;
    if (*p == '\'')
        *p++ = '\0';
}
```

The full token dispatch in `tokenize()` should now be:

```c
if (*p == '"') {
    p++;
    cmd->argv[cmd->argc++] = p;
    while (*p && *p != '"')
        p++;
    if (*p == '"')
        *p++ = '\0';
} else if (*p == '\'') {
    p++;
    cmd->argv[cmd->argc++] = p;
    while (*p && *p != '\'')
        p++;
    if (*p == '\'')
        *p++ = '\0';
} else {
    cmd->argv[cmd->argc++] = p;
    while (*p && *p != ' ' && *p != '\t')
        p++;
    if (*p)
        *p++ = '\0';
}
```

**Step 2: Protect single-quoted regions from variable expansion**

In `expand_vars()`, skip `$` inside single quotes. Add quote tracking:

```c
static void expand_vars(char *line) {
    char *src = line;
    char *dst = expand_buf;
    char *end = expand_buf + MAX_LINE - 1;
    int in_single_quote = 0;

    while (*src && dst < end) {
        if (*src == '\'' && !in_single_quote) {
            in_single_quote = 1;
            *dst++ = *src++;
        } else if (*src == '\'' && in_single_quote) {
            in_single_quote = 0;
            *dst++ = *src++;
        } else if (*src == '$' && !in_single_quote) {
            /* ... existing $ expansion code ... */
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    strcpy(line, expand_buf);
}
```

**Step 3: Update `parse_pipeline()` and `parse_line()` quote tracking**

The existing `in_q` variable only tracks double quotes. It needs to also track single quotes to avoid splitting on `|`, `;`, `&&`, `||` inside single-quoted strings.

In `parse_pipeline()` around line 239, change the quote detection:

```c
int in_dq = 0, in_sq = 0;
while (*scan) {
    if (*scan == '"' && !in_sq) { in_dq = !in_dq; scan++; continue; }
    if (*scan == '\'' && !in_dq) { in_sq = !in_sq; scan++; continue; }
    if (!in_dq && !in_sq && *scan == '|' && *(scan+1) != '|') {
        pipe_pos = scan;
        break;
    }
    scan++;
}
```

Similarly in `parse_line()` around line 292:

```c
int in_dq = 0, in_sq = 0;
/* ... */
if (*p == '"' && !in_sq) { in_dq = !in_dq; p++; continue; }
if (*p == '\'' && !in_dq) { in_sq = !in_sq; p++; continue; }
if (in_dq || in_sq) { p++; continue; }
```

**Step 4: Build, test, commit**

```bash
make clean && make
# In QEMU: echo '$PATH' → should print literal "$PATH"
git add bin/sbush/parse.c
git commit -m "ENH: Added single-quote support to sbush parser"
```

---

## Task 4: Backslash escape handling

**Files:**
- Modify: `bin/sbush/parse.c:tokenize()`

Inside double-quoted strings, `\"` should produce a literal `"`, `\\` should produce `\`, and `\n` should produce a newline.

**Step 1: Add escape handling in the double-quote branch of `tokenize()`**

Replace the double-quote branch:

```c
if (*p == '"') {
    p++;
    char *start = p;
    char *write = p; /* in-place de-escaping */
    while (*p && *p != '"') {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case 'n':  *write++ = '\n'; break;
                case 't':  *write++ = '\t'; break;
                case '\\': *write++ = '\\'; break;
                case '"':  *write++ = '"';  break;
                case '$':  *write++ = '$';  break;
                default:   *write++ = '\\'; *write++ = *p; break;
            }
            p++;
        } else {
            *write++ = *p++;
        }
    }
    *write = '\0';
    if (*p == '"') p++;
    cmd->argv[cmd->argc++] = start;
}
```

**Step 2: Build, test, commit**

```bash
make clean && make
# In QEMU: echo "hello\"world" → should print: hello"world
git add bin/sbush/parse.c
git commit -m "ENH: Added backslash escape handling in double-quoted strings"
```

---

## Task 5: File redirections (`>`, `>>`, `<`, `2>`, `2>&1`)

**Files:**
- Modify: `bin/sbush/sbush.h` (add redirection fields to `struct command`)
- Modify: `bin/sbush/parse.c:tokenize()` (parse redirection operators)
- Modify: `bin/sbush/exec.c:execute_pipeline()` (apply redirections in child before exec)

**Step 1: Add redirection fields to `struct command`**

In `sbush.h`, add to `struct command`:

```c
struct command {
    char *argv[MAX_ARGS];
    int   argc;
    char *redir_in;       /* < file */
    char *redir_out;      /* > file */
    char *redir_append;   /* >> file */
    char *redir_err;      /* 2> file */
    int   redir_err_to_out; /* 2>&1 */
};
```

**Step 2: Parse redirections in `tokenize()`**

After building the argv array, scan for redirection tokens and remove them from argv. Add a post-processing step at the end of `tokenize()`, before `return cmd->argc;`:

```c
/* Extract redirections from argv */
cmd->redir_in = NULL;
cmd->redir_out = NULL;
cmd->redir_append = NULL;
cmd->redir_err = NULL;
cmd->redir_err_to_out = 0;

int j = 0;
for (int i = 0; i < cmd->argc; i++) {
    if (strcmp(cmd->argv[i], "<") == 0 && i + 1 < cmd->argc) {
        cmd->redir_in = cmd->argv[++i];
    } else if (strcmp(cmd->argv[i], ">>") == 0 && i + 1 < cmd->argc) {
        cmd->redir_append = cmd->argv[++i];
    } else if (strcmp(cmd->argv[i], ">") == 0 && i + 1 < cmd->argc) {
        cmd->redir_out = cmd->argv[++i];
    } else if (strcmp(cmd->argv[i], "2>&1") == 0) {
        cmd->redir_err_to_out = 1;
    } else if (strcmp(cmd->argv[i], "2>") == 0 && i + 1 < cmd->argc) {
        cmd->redir_err = cmd->argv[++i];
    } else {
        cmd->argv[j++] = cmd->argv[i];
    }
}
cmd->argc = j;
cmd->argv[j] = NULL;
```

**Step 3: Apply redirections in the child process**

In `exec.c:execute_pipeline()`, in the child block (after `pid == 0`, after pipe setup, before the builtin/exec logic), add:

```c
/* Apply redirections */
struct command *c = &pl->cmds[i];
if (c->redir_in) {
    int fd = open(c->redir_in, 0); /* O_RDONLY */
    if (fd < 0) {
        dprintf(2, "sbush: %s: No such file or directory\n", c->redir_in);
        exit(1);
    }
    dup2(fd, STDIN_FILENO);
    close(fd);
}
if (c->redir_out) {
    int fd = open(c->redir_out, 0x41, 0644); /* O_WRONLY|O_CREAT|O_TRUNC */
    if (fd < 0) {
        dprintf(2, "sbush: %s: Cannot create file\n", c->redir_out);
        exit(1);
    }
    dup2(fd, STDOUT_FILENO);
    close(fd);
}
if (c->redir_append) {
    int fd = open(c->redir_append, 0x441, 0644); /* O_WRONLY|O_CREAT|O_APPEND */
    if (fd < 0) {
        dprintf(2, "sbush: %s: Cannot open file\n", c->redir_append);
        exit(1);
    }
    dup2(fd, STDOUT_FILENO);
    close(fd);
}
if (c->redir_err) {
    int fd = open(c->redir_err, 0x41, 0644); /* O_WRONLY|O_CREAT|O_TRUNC */
    if (fd < 0) {
        dprintf(2, "sbush: %s: Cannot create file\n", c->redir_err);
        exit(1);
    }
    dup2(fd, STDERR_FILENO);
    close(fd);
}
if (c->redir_err_to_out) {
    dup2(STDOUT_FILENO, STDERR_FILENO);
}
```

**NOTE on open flags:** The exact flag values depend on what `fcntl.h` defines. The kernel's `SYS_openat` handler needs to support O_CREAT and O_TRUNC for output redirections to work. If the tarfs is read-only, redirections to files will fail — they only work if a writable filesystem is added. For now, redirections will work for reading from tarfs files (`< /etc/rc`) but not for writing.

**Step 4: Build, test, commit**

```bash
make clean && make
# In QEMU: cat < /etc/rc  → should print rc file contents
git add bin/sbush/sbush.h bin/sbush/parse.c bin/sbush/exec.c
git commit -m "ENH: Added file redirection support to sbush"
```

---

## Task 6: Add `jobs`, `fg`, `bg` builtins (foundation)

**Files:**
- Modify: `bin/sbush/sbush.h` (job table structures)
- Create: `bin/sbush/jobs.c` (job tracking)
- Modify: `bin/sbush/builtin.c` (register new builtins)
- Modify: `bin/sbush/exec.c` (register background jobs)

**NOTE:** Full job control (SIGTSTP suspend/resume) requires kernel signal delivery (Workstream 4). This task implements the data structures and basic `jobs` listing. `fg`/`bg` will work once signals are available.

**Step 1: Add job table to `sbush.h`**

```c
/* Job control */
#define MAX_JOBS 16

enum job_state { JOB_RUNNING, JOB_STOPPED, JOB_DONE };

struct job {
    int        id;         /* job number [1..N] */
    pid_t      pid;        /* lead process PID */
    enum job_state state;
    char       cmd[128];   /* command string for display */
};

/* jobs.c */
int   job_add(pid_t pid, const char *cmd);
void  job_remove(pid_t pid);
void  job_set_state(pid_t pid, enum job_state state);
struct job *job_find(int id);
struct job *job_find_pid(pid_t pid);
void  job_list(void);
void  job_reap(void);
```

**Step 2: Create `bin/sbush/jobs.c`**

```c
#include "sbush.h"

static struct job jobs[MAX_JOBS];
static int next_job_id = 1;

int job_add(pid_t pid, const char *cmd) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].id = next_job_id++;
            jobs[i].pid = pid;
            jobs[i].state = JOB_RUNNING;
            strncpy(jobs[i].cmd, cmd, 127);
            jobs[i].cmd[127] = '\0';
            return jobs[i].id;
        }
    }
    return -1;
}

void job_remove(pid_t pid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pid == pid) {
            jobs[i].pid = 0;
            jobs[i].id = 0;
            return;
        }
    }
}

void job_set_state(pid_t pid, enum job_state state) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pid == pid) {
            jobs[i].state = state;
            return;
        }
    }
}

struct job *job_find(int id) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].id == id && jobs[i].pid != 0)
            return &jobs[i];
    return NULL;
}

struct job *job_find_pid(pid_t pid) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].pid == pid)
            return &jobs[i];
    return NULL;
}

void job_list(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pid == 0) continue;
        const char *state = "Running";
        if (jobs[i].state == JOB_STOPPED) state = "Stopped";
        if (jobs[i].state == JOB_DONE) state = "Done";
        printf("[%d] %s\t%s\n", jobs[i].id, state, jobs[i].cmd);
    }
}

void job_reap(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pid == 0) continue;
        int status;
        pid_t ret = waitpid(jobs[i].pid, &status, WNOHANG);
        if (ret > 0) {
            jobs[i].state = JOB_DONE;
            printf("[%d] Done\t%s\n", jobs[i].id, jobs[i].cmd);
            jobs[i].pid = 0;
        }
    }
}
```

**Step 3: Register builtins in `builtin.c`**

Add `jobs`, `fg`, `bg` to `is_builtin()`:

```c
strcmp(cmd, "jobs") == 0 ||
strcmp(cmd, "fg") == 0 ||
strcmp(cmd, "bg") == 0 ||
```

Add handlers to `run_builtin()`:

```c
if (strcmp(cmd->argv[0], "jobs") == 0) {
    job_list();
    return 0;
}

if (strcmp(cmd->argv[0], "fg") == 0) {
    int id = cmd->argc > 1 ? atoi(cmd->argv[1]) : -1;
    struct job *j = (id > 0) ? job_find(id) : NULL;
    if (!j) {
        printf("fg: no such job\n");
        return 1;
    }
    /* Wait for the job */
    int status;
    waitpid(j->pid, &status, 0);
    job_remove(j->pid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

if (strcmp(cmd->argv[0], "bg") == 0) {
    /* bg requires SIGCONT which needs kernel signal delivery */
    printf("bg: not yet implemented (needs kernel signal support)\n");
    return 1;
}
```

**Step 4: Register background jobs in `exec.c`**

In `execute_pipeline()`, when `pl->background` is true (around line 70):

```c
if (pl->background) {
    last_bg_pid = pids[pl->ncmds - 1];
    /* Build command string for job display */
    char jobcmd[128] = "";
    for (int j = 0; j < pl->cmds[0].argc && strlen(jobcmd) < 100; j++) {
        if (j > 0) strcat(jobcmd, " ");
        strcat(jobcmd, pl->cmds[0].argv[j]);
    }
    int jid = job_add(last_bg_pid, jobcmd);
    printf("[%d] %d\n", jid, (int)last_bg_pid);
    if (prev_fd >= 0) close(prev_fd);
    return 0;
}
```

Replace `reap_zombies()` in `sbush.c` with `job_reap()`:

```c
// was: reap_zombies();
job_reap();
/* still reap non-job zombies */
while (waitpid(-1, NULL, WNOHANG) > 0)
    ;
```

**Step 5: Build, test, commit**

```bash
make clean && make
# In QEMU: sleep 5 &  → prints [1] <pid>
#           jobs       → prints [1] Running  sleep 5
git add bin/sbush/sbush.h bin/sbush/jobs.c bin/sbush/builtin.c bin/sbush/exec.c bin/sbush/sbush.c
git commit -m "ENH: Added jobs, fg, bg builtins with job tracking"
```
