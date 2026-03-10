# Shell Features Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add six POSIX shell features: tilde expansion, aliases, `exec` builtin, `umask` builtin, glob character classes, and arithmetic bitwise/ternary operators.

**Architecture:** All changes are in `bin/sbush/` (shell userspace). Each feature is self-contained. Tilde expansion goes in the variable expansion pass. Aliases get a new lookup table consulted before builtin/function/PATH resolution. `exec` and `umask` are new builtins. Glob character classes extend the existing `glob_match()` pattern matcher. Arithmetic operators extend the existing recursive descent evaluator. The `umask` builtin also requires fixing the kernel's stub handler.

**Tech Stack:** C (userspace, freestanding libc, riscv64-unknown-elf-gcc)

---

## Task 1: Tilde Expansion

**Files:**
- Modify: `bin/sbush/parse.c:386-489` (expand_vars — add tilde handling before `$` expansion)

### Step 1: Add tilde expansion to `expand_vars()`

Tilde expansion applies to `~` at the start of a word (i.e., at the start of the line, or after whitespace/separator). In `expand_vars()` at line 386, we need to handle `~` when it appears at a word boundary.

The simplest correct approach: after the existing `while (*src && dst < end)` loop processes `$` and backticks, add a second pass that handles tilde. But it's cleaner to handle it inline.

In `expand_vars()`, inside the main `while` loop (after the single-quote and `$` checks), add a `~` check. The tilde must be at the start of a word (preceded by nothing, whitespace, `=`, or `:`):

Add this block in `expand_vars()` right before the final `else` clause at line 483 (the `*dst++ = *src++` catch-all):

```c
        } else if (*src == '~' && !in_sq) {
            /* Tilde expansion: ~ or ~/ at start of word */
            int at_word_start = (src == line) ||
                                (src > line && (src[-1] == ' '  || src[-1] == '\t' ||
                                                src[-1] == '='  || src[-1] == ':'));
            if (at_word_start && (src[1] == '\0' || src[1] == '/' ||
                                   src[1] == ' '  || src[1] == '\t' ||
                                   src[1] == ':'  || src[1] == '\n')) {
                char *home = var_get("HOME");
                if (home) {
                    while (*home && dst < end)
                        *dst++ = *home++;
                }
                src++; /* skip ~ */
            } else {
                *dst++ = *src++;
            }
```

Note: `expand_vars` works on the `line` buffer and `src` is a pointer into it, so `src == line` correctly detects start-of-line. For mid-line tildes after whitespace, `src[-1]` is safe because `src > line`.

### Step 2: Also handle tilde in `glob_expand_token` check

In `glob_expand_token()` at line 620, the function checks for `*` and `?` to decide whether to glob. The `[` character will be added in Task 5. No changes needed here for tilde — tilde expansion happens before globbing.

### Step 3: Build

Run: `source tools/sourceme.sh && make clean && make`

Expected: Clean build.

### Step 4: Test in QEMU

Run: `make qemu`

```
sbush$ export HOME=/
sbush$ echo ~
/
sbush$ echo ~/bin
/bin
sbush$ echo hello~world
hello~world
sbush$ echo ~nonexistent
~nonexistent
```

**What to verify:**
1. `~` alone expands to `$HOME`
2. `~/bin` expands to `$HOME/bin`
3. `~` in the middle of a word is literal
4. `~username` (unsupported) is left as-is

### Step 5: Commit

```
ENH: Added tilde expansion for HOME directory in shell
```

---

## Task 2: Alias Support

**Files:**
- Modify: `bin/sbush/sbush.h` (add alias table declarations)
- Modify: `bin/sbush/builtin.c:161-186` (add alias/unalias to is_builtin)
- Modify: `bin/sbush/builtin.c:268-521` (add alias/unalias handlers to run_builtin)
- Modify: `bin/sbush/parse.c` (add alias expansion in `expand_and_parse_pipeline`)

### Step 1: Add alias table to `sbush.h`

Add after the `MAX_EXEC_DEPTH` define (line 87):

```c
/* Alias table */
#define MAX_ALIASES 32

struct alias_entry {
    char name[64];
    char value[256];
};

/* alias.c or inline in builtin.c */
char *alias_get(const char *name);
void  alias_set(const char *name, const char *value);
void  alias_unset(const char *name);
void  alias_list(void);
```

### Step 2: Implement alias table in `builtin.c`

Add at the top of `builtin.c` (after the includes):

```c
/* -----------------------------------------------------------------------
 * Alias table
 * ---------------------------------------------------------------------- */
static struct alias_entry aliases[MAX_ALIASES];
static int nalias = 0;

char *alias_get(const char *name) {
    for (int i = 0; i < nalias; i++)
        if (strcmp(aliases[i].name, name) == 0)
            return aliases[i].value;
    return NULL;
}

void alias_set(const char *name, const char *value) {
    for (int i = 0; i < nalias; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            strncpy(aliases[i].value, value, 255);
            aliases[i].value[255] = '\0';
            return;
        }
    }
    if (nalias >= MAX_ALIASES) return;
    strncpy(aliases[nalias].name, name, 63);
    aliases[nalias].name[63] = '\0';
    strncpy(aliases[nalias].value, value, 255);
    aliases[nalias].value[255] = '\0';
    nalias++;
}

void alias_unset(const char *name) {
    for (int i = 0; i < nalias; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            nalias--;
            if (i < nalias)
                aliases[i] = aliases[nalias];
            return;
        }
    }
}

void alias_list(void) {
    for (int i = 0; i < nalias; i++)
        printf("alias %s='%s'\n", aliases[i].name, aliases[i].value);
}
```

### Step 3: Register `alias` and `unalias` as builtins

In `is_builtin()` at line 185, add before the closing parenthesis:

```c
            strcmp(cmd, "alias") == 0 ||
            strcmp(cmd, "unalias") == 0);
```

In `run_builtin()`, add before the final `return 1;` at line 520:

```c
    if (strcmp(cmd->argv[0], "alias") == 0) {
        if (cmd->argc == 1) {
            alias_list();
            return 0;
        }
        for (int i = 1; i < cmd->argc; i++) {
            char *eq = strchr(cmd->argv[i], '=');
            if (eq) {
                *eq = '\0';
                alias_set(cmd->argv[i], eq + 1);
                *eq = '=';
            } else {
                char *val = alias_get(cmd->argv[i]);
                if (val)
                    printf("alias %s='%s'\n", cmd->argv[i], val);
                else {
                    dprintf(2, "sbush: alias: %s: not found\n", cmd->argv[i]);
                    return 1;
                }
            }
        }
        return 0;
    }

    if (strcmp(cmd->argv[0], "unalias") == 0) {
        for (int i = 1; i < cmd->argc; i++)
            alias_unset(cmd->argv[i]);
        return 0;
    }
```

### Step 4: Add alias expansion in the parse path

Alias expansion replaces the first word of a simple command with the alias value. This should happen in `expand_and_parse_pipeline()` in `parse.c`, after variable expansion but before tokenization.

Find `expand_and_parse_pipeline()` in `parse.c`. After `expand_vars(seg)` is called and before the line is tokenized, extract the first word and check for an alias. If found, substitute it.

Add alias expansion logic: after `expand_vars` runs on the segment, scan for the first non-whitespace word. If it matches an alias, replace it with the alias value in the buffer.

```c
    /* Alias expansion — replace first word if it matches an alias */
    {
        char *p = seg;
        while (*p == ' ' || *p == '\t') p++;
        char first[64];
        int fi = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' &&
               *p != ';' && *p != '|' && *p != '<' && *p != '>' && fi < 63)
            first[fi++] = *p++;
        first[fi] = '\0';
        if (fi > 0) {
            char *aval = alias_get(first);
            if (aval) {
                static char alias_buf[MAX_LINE];
                int alen = strlen(aval);
                int rest = strlen(p);
                if (alen + rest < MAX_LINE - 1) {
                    memcpy(alias_buf, aval, alen);
                    memcpy(alias_buf + alen, p, rest);
                    alias_buf[alen + rest] = '\0';
                    strcpy(seg, alias_buf);
                }
            }
        }
    }
```

### Step 5: Build

Run: `source tools/sourceme.sh && make clean && make`

Expected: Clean build.

### Step 6: Test in QEMU

```
sbush$ alias ll=ls
sbush$ alias
alias ll='ls'
sbush$ ll /bin
<directory listing>
sbush$ unalias ll
sbush$ ll /bin
sbush: ll: command not found
```

**What to verify:**
1. `alias name=value` stores the alias
2. `alias` with no args lists all aliases
3. Using `ll` expands to `ls` and runs correctly
4. `unalias ll` removes it
5. After unalias, `ll` is not found

### Step 7: Commit

```
ENH: Added alias and unalias shell builtins
```

---

## Task 3: `exec` Builtin

**Files:**
- Modify: `bin/sbush/builtin.c:161-186` (add exec to is_builtin)
- Modify: `bin/sbush/builtin.c:268-521` (add exec handler)

### Step 1: Register `exec` as a builtin

In `is_builtin()`, add:
```c
            strcmp(cmd, "exec") == 0 ||
```

### Step 2: Implement exec handler in `run_builtin()`

Add before the final `return 1;`:

```c
    if (strcmp(cmd->argv[0], "exec") == 0) {
        /* exec with no args but redirections: apply to the shell itself.
         * Since in-process builtins don't get redirections applied,
         * we handle it specially here by applying redirections. */
        if (cmd->argc == 1) {
            /* No command — apply redirections to the shell.
             * This is handled by the caller when exec returns. */
            return 0;
        }
        /* exec with a command: replace the shell with execve */
        char *path = resolve_path(cmd->argv[1]);
        if (!path) {
            dprintf(2, "sbush: exec: %s: command not found\n", cmd->argv[1]);
            return 127;
        }
        char **envp = build_environ();
        execve(path, cmd->argv + 1, envp);
        /* If we get here, execve failed */
        dprintf(2, "sbush: exec: %s: exec failed\n", cmd->argv[1]);
        return 126;
    }
```

### Step 3: Handle `exec` with redirections

The `exec` builtin is special — when it's used with redirections but no command (like `exec >log.txt`), the redirections should apply to the shell process itself, not a child. Currently, `execute_pipeline()` at line 15 runs single builtins in-process but skips redirection application (redirections only happen in child processes).

Modify the in-process builtin path in `exec.c` at line 15 to detect `exec` and apply redirections to the shell. Add after line 17 (the existing `run_builtin` call):

Replace this block (lines 14-19):
```c
    /* single builtin — run in-process (unless it's in a pipe or has redirections) */
    if (pl->ncmds == 1 && !pl->background && is_builtin(pl->cmds[0].argv[0]) &&
        !pl->cmds[0].redir_in && !pl->cmds[0].redir_out &&
        !pl->cmds[0].redir_append && !pl->cmds[0].redir_err) {
        return run_builtin(&pl->cmds[0]);
    }
```

With:
```c
    /* single builtin — run in-process (unless it's in a pipe or has redirections) */
    if (pl->ncmds == 1 && !pl->background && is_builtin(pl->cmds[0].argv[0])) {
        struct command *c = &pl->cmds[0];
        int has_redir = c->redir_in || c->redir_out || c->redir_append || c->redir_err;

        if (strcmp(c->argv[0], "exec") == 0 && has_redir) {
            /* exec with redirections: apply to the shell itself */
            if (c->redir_in) {
                int fd = open(c->redir_in, O_RDONLY);
                if (fd < 0) {
                    dprintf(2, "sbush: %s: No such file or directory\n", c->redir_in);
                    return 1;
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
            if (c->redir_out) {
                int fd = open(c->redir_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    dprintf(2, "sbush: %s: Cannot create file\n", c->redir_out);
                    return 1;
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
            if (c->redir_append) {
                int fd = open(c->redir_append, O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fd < 0) {
                    dprintf(2, "sbush: %s: Cannot open file\n", c->redir_append);
                    return 1;
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
            if (c->redir_err) {
                int fd = open(c->redir_err, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    dprintf(2, "sbush: %s: Cannot create file\n", c->redir_err);
                    return 1;
                }
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
            if (c->redir_err_to_out) {
                dup2(STDOUT_FILENO, STDERR_FILENO);
            }
            return run_builtin(c);
        }

        if (!has_redir)
            return run_builtin(c);
    }
```

Make sure to add `#include <fcntl.h>` at the top of `exec.c` if it isn't already there (it is — line 2).

### Step 4: Build

Run: `source tools/sourceme.sh && make clean && make`

Expected: Clean build.

### Step 5: Test in QEMU

```
sbush$ exec echo hello from exec
hello from exec
```

Note: After `exec echo hello`, the shell is replaced by echo, so the shell exits. For testing in the OS where the shell is PID 1 or the init shell, be careful — `exec` replacing the shell means the shell is gone. Test instead with redirections:

```
sbush$ exec >/tmp/testlog.txt
sbush$ echo this goes to the file
sbush$ exec >/dev/console
sbush$ cat /tmp/testlog.txt
this goes to the file
```

**What to verify:**
1. `exec >file` redirects the shell's stdout to the file
2. Subsequent echo output goes to the file
3. `exec >/dev/console` restores output to the console
4. `exec <cmd>` replaces the shell with the command

### Step 6: Commit

```
ENH: Added exec builtin with redirection support
```

---

## Task 4: `umask` Builtin

**Files:**
- Modify: `kernel/trap.c:1988-1991` (fix umask kernel handler)
- Modify: `kernel/include/proc.h` (add umask field to proc struct)
- Modify: `bin/sbush/builtin.c` (add umask builtin)

### Step 1: Add `umask` field to the process struct

In `kernel/include/proc.h`, in `struct proc`, add after the `cwd` field:

```c
  unsigned int umask_val;          /* file creation mask (default 022) */
```

### Step 2: Initialize umask in process creation

Find where new processes are initialized in the kernel (likely in the fork/clone handler). Set `umask_val = 0022` for the init process, and copy from parent on fork.

Search for where `cwd` is copied on fork — `umask_val` should be copied the same way.

### Step 3: Fix the kernel umask handler

Replace the stub at `kernel/trap.c:1988-1991`:

```c
static long sys_umask_handler(struct trapframe *tf) {
    (void)tf;
    return (long)0022;
}
```

With:

```c
static long sys_umask_handler(struct trapframe *tf) {
    unsigned int new_mask = (unsigned int)tf->a0 & 0777;
    unsigned int old_mask = current_proc->umask_val;
    current_proc->umask_val = new_mask;
    return (long)old_mask;
}
```

### Step 4: Wire umask into file creation

Wherever the kernel creates files (openat with O_CREAT, mkdirat, mknodat), the mode argument should be masked with `~current_proc->umask_val`. Search for `O_CREAT` handling in `kernel/trap.c` and apply `mode &= ~current_proc->umask_val;` before passing to the filesystem.

### Step 5: Register `umask` as a shell builtin

In `is_builtin()`, add:
```c
            strcmp(cmd, "umask") == 0 ||
```

In `run_builtin()`, add:

```c
    if (strcmp(cmd->argv[0], "umask") == 0) {
        if (cmd->argc == 1) {
            /* Print current umask in octal */
            unsigned int m = umask(0);
            umask(m); /* restore it */
            printf("%04o\n", m);
            return 0;
        }
        /* Parse octal argument */
        const char *s = cmd->argv[1];
        unsigned int mask = 0;
        while (*s >= '0' && *s <= '7')
            mask = mask * 8 + (*s++ - '0');
        umask(mask);
        return 0;
    }
```

### Step 6: Build

Run: `source tools/sourceme.sh && make clean && make`

Expected: Clean build.

### Step 7: Test in QEMU

```
sbush$ umask
0022
sbush$ umask 077
sbush$ umask
0077
sbush$ umask 022
sbush$ umask
0022
```

**What to verify:**
1. `umask` with no args prints current mask in 4-digit octal
2. `umask 077` sets the mask
3. Reading it back returns the newly set value

### Step 8: Commit

```
ENH: Added working umask syscall and shell builtin
```

---

## Task 5: Glob Character Classes

**Files:**
- Modify: `bin/sbush/parse.c:575-597` (glob_match — add `[...]` handling)
- Modify: `bin/sbush/parse.c:619-624` (glob_expand_token — detect `[` as glob char)

### Step 1: Extend `glob_match()` to handle `[...]`

Replace the entire `glob_match()` function (lines 575-597):

```c
int glob_match(const char *pat, const char *name) {
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return 1;
            while (*name) {
                if (glob_match(pat, name))
                    return 1;
                name++;
            }
            return 0;
        } else if (*pat == '?') {
            if (!*name) return 0;
            pat++;
            name++;
        } else if (*pat == '[') {
            if (!*name) return 0;
            pat++; /* skip '[' */
            int negate = 0;
            if (*pat == '!' || *pat == '^') {
                negate = 1;
                pat++;
            }
            int matched = 0;
            /* ']' as first char after [, [!, [^ is literal */
            int first = 1;
            while (*pat && (*pat != ']' || first)) {
                first = 0;
                char lo = *pat++;
                if (*pat == '-' && pat[1] && pat[1] != ']') {
                    pat++; /* skip '-' */
                    char hi = *pat++;
                    if ((unsigned char)*name >= (unsigned char)lo &&
                        (unsigned char)*name <= (unsigned char)hi)
                        matched = 1;
                } else {
                    if (*name == lo)
                        matched = 1;
                }
            }
            if (*pat == ']') pat++;
            if (negate) matched = !matched;
            if (!matched) return 0;
            name++;
        } else {
            if (*pat != *name) return 0;
            pat++;
            name++;
        }
    }
    return *name == '\0';
}
```

### Step 2: Detect `[` as a glob character in `glob_expand_token`

In `glob_expand_token()` at line 620, add `[` detection:

```c
    if (!strchr(token, '*') && !strchr(token, '?') && !strchr(token, '[')) {
```

Also in `glob_expand_cmd()` at line 693, add `[` detection:

```c
        if (strchr(cmd->argv[i], '*') || strchr(cmd->argv[i], '?') || strchr(cmd->argv[i], '[')) {
```

### Step 3: Build

Run: `source tools/sourceme.sh && make clean && make`

Expected: Clean build.

### Step 4: Test in QEMU

```
sbush$ echo /bin/[a-z]*
<files in /bin starting with a-z>
sbush$ echo /bin/[!s]*
<files in /bin NOT starting with s>
```

Create test files to verify ranges:
```
sbush$ echo test > /tmp/file_a
sbush$ echo test > /tmp/file_b
sbush$ echo test > /tmp/file_c
sbush$ echo test > /tmp/file_1
sbush$ echo /tmp/file_[a-c]
/tmp/file_a /tmp/file_b /tmp/file_c
sbush$ echo /tmp/file_[!a-c]
/tmp/file_1
```

**What to verify:**
1. `[a-z]` matches lowercase letters
2. `[abc]` matches specific characters
3. `[!abc]` and `[^abc]` match characters NOT in the set
4. `[a-c]` range works correctly
5. Non-matching patterns pass through as literals

### Step 5: Commit

```
ENH: Added glob character class patterns to shell
```

---

## Task 6: Arithmetic Bitwise and Ternary Operators

**Files:**
- Modify: `bin/sbush/parse.c:86-227` (arithmetic evaluator — add new precedence levels)

### Step 1: Restructure the arithmetic parser

The current precedence (lowest to highest):
1. comparison (`==`, `!=`, `<`, `>`, `<=`, `>=`) — `arith_expr()`
2. addition (`+`, `-`) — `arith_add()`
3. multiplication (`*`, `/`, `%`) — `arith_term()`
4. unary (`-`, `+`, `!`) — `arith_factor()`

The new precedence should be (lowest to highest):
1. **ternary** (`? :`) — `arith_ternary()` (NEW — becomes the entry point)
2. **bitwise OR** (`|`) — `arith_bitor()` (NEW)
3. **bitwise XOR** (`^`) — `arith_bitxor()` (NEW)
4. **bitwise AND** (`&`) — `arith_bitand()` (NEW)
5. comparison (`==`, `!=`, `<`, `>`, `<=`, `>=`) — `arith_cmp()` (renamed from arith_expr)
6. **shift** (`<<`, `>>`) — `arith_shift()` (NEW)
7. addition (`+`, `-`) — `arith_add()`
8. multiplication (`*`, `/`, `%`) — `arith_term()`
9. unary (`-`, `+`, `!`, `~`) — `arith_factor()` (add `~`)

Replace the entire arithmetic section (lines 86-227) with:

```c
/* -----------------------------------------------------------------------
 * Arithmetic expansion: $((...))
 * Recursive descent with full operator precedence.
 * ---------------------------------------------------------------------- */
static const char *arith_p;

static long arith_ternary(void);

static void arith_skip_ws(void) {
    while (*arith_p == ' ' || *arith_p == '\t')
        arith_p++;
}

static long arith_factor(void) {
    arith_skip_ws();
    if (*arith_p == '(') {
        arith_p++;
        long v = arith_ternary();
        arith_skip_ws();
        if (*arith_p == ')') arith_p++;
        return v;
    }
    if (*arith_p == '-') { arith_p++; return -arith_factor(); }
    if (*arith_p == '+') { arith_p++; return arith_factor(); }
    if (*arith_p == '!') { arith_p++; return !arith_factor(); }
    if (*arith_p == '~') { arith_p++; return ~arith_factor(); }
    if (*arith_p == '$') {
        arith_p++;
        if (*arith_p == '?') { arith_p++; return (long)last_exit_status; }
        if (*arith_p == '#') { arith_p++; return (long)positional_count; }
        if (*arith_p >= '0' && *arith_p <= '9') {
            int idx = *arith_p - '0'; arith_p++;
            char *val = positional_get(idx);
            return val ? atol(val) : 0;
        }
        char name[64];
        int i = 0;
        while (is_name_char(*arith_p) && i < 63)
            name[i++] = *arith_p++;
        name[i] = '\0';
        char *val = var_get(name);
        return val ? atol(val) : 0;
    }
    if (*arith_p >= '0' && *arith_p <= '9') {
        long v = 0;
        while (*arith_p >= '0' && *arith_p <= '9')
            v = v * 10 + (*arith_p++ - '0');
        return v;
    }
    /* bare variable name */
    if (is_name_char(*arith_p)) {
        char name[64];
        int i = 0;
        while (is_name_char(*arith_p) && i < 63)
            name[i++] = *arith_p++;
        name[i] = '\0';
        char *val = var_get(name);
        return val ? atol(val) : 0;
    }
    return 0;
}

static long arith_term(void) {
    long v = arith_factor();
    while (1) {
        arith_skip_ws();
        if (*arith_p == '*') { arith_p++; v *= arith_factor(); }
        else if (*arith_p == '/' && arith_p[1] != '/') {
            arith_p++;
            long d = arith_factor();
            v = d ? v / d : 0;
        }
        else if (*arith_p == '%') { arith_p++; long d = arith_factor(); v = d ? v % d : 0; }
        else break;
    }
    return v;
}

static long arith_add(void) {
    long v = arith_term();
    while (1) {
        arith_skip_ws();
        if (*arith_p == '+') { arith_p++; v += arith_term(); }
        else if (*arith_p == '-') { arith_p++; v -= arith_term(); }
        else break;
    }
    return v;
}

static long arith_shift(void) {
    long v = arith_add();
    while (1) {
        arith_skip_ws();
        if (arith_p[0] == '<' && arith_p[1] == '<') {
            arith_p += 2; v <<= arith_add();
        } else if (arith_p[0] == '>' && arith_p[1] == '>' && arith_p[2] != '=') {
            arith_p += 2; v >>= arith_add();
        } else break;
    }
    return v;
}

static long arith_cmp(void) {
    long v = arith_shift();
    while (1) {
        arith_skip_ws();
        if (arith_p[0] == '=' && arith_p[1] == '=') { arith_p += 2; v = (v == arith_shift()); }
        else if (arith_p[0] == '!' && arith_p[1] == '=') { arith_p += 2; v = (v != arith_shift()); }
        else if (arith_p[0] == '<' && arith_p[1] == '=') { arith_p += 2; v = (v <= arith_shift()); }
        else if (arith_p[0] == '>' && arith_p[1] == '=') { arith_p += 2; v = (v >= arith_shift()); }
        else if (arith_p[0] == '<' && arith_p[1] != '<') { arith_p++; v = (v < arith_shift()); }
        else if (arith_p[0] == '>' && arith_p[1] != '>') { arith_p++; v = (v > arith_shift()); }
        else break;
    }
    return v;
}

static long arith_bitand(void) {
    long v = arith_cmp();
    while (1) {
        arith_skip_ws();
        /* & but not && */
        if (*arith_p == '&' && arith_p[1] != '&') { arith_p++; v &= arith_cmp(); }
        else break;
    }
    return v;
}

static long arith_bitxor(void) {
    long v = arith_bitand();
    while (1) {
        arith_skip_ws();
        if (*arith_p == '^') { arith_p++; v ^= arith_bitand(); }
        else break;
    }
    return v;
}

static long arith_bitor(void) {
    long v = arith_bitxor();
    while (1) {
        arith_skip_ws();
        /* | but not || */
        if (*arith_p == '|' && arith_p[1] != '|') { arith_p++; v |= arith_bitxor(); }
        else break;
    }
    return v;
}

static long arith_ternary(void) {
    long cond = arith_bitor();
    arith_skip_ws();
    if (*arith_p == '?') {
        arith_p++;
        long then_val = arith_ternary();
        arith_skip_ws();
        if (*arith_p == ':') arith_p++;
        long else_val = arith_ternary();
        return cond ? then_val : else_val;
    }
    return cond;
}
```

### Step 2: Update `expand_arith()` to call `arith_ternary()`

In `expand_arith()` at line 211 (now adjusted after replacement), change:

```c
    arith_p = arith_buf;
    long result = arith_expr();
```

To:

```c
    arith_p = arith_buf;
    long result = arith_ternary();
```

### Step 3: Remove the old `arith_expr` forward declaration

The old forward declaration at line 96 (`static long arith_expr(void);`) should be changed to:

```c
static long arith_ternary(void);
```

(This is already included in the replacement block above.)

### Step 4: Build

Run: `source tools/sourceme.sh && make clean && make`

Expected: Clean build.

### Step 5: Test in QEMU

```
sbush$ echo $((3 & 1))
1
sbush$ echo $((3 | 4))
7
sbush$ echo $((5 ^ 3))
6
sbush$ echo $((~0))
-1
sbush$ echo $((1 << 4))
16
sbush$ echo $((32 >> 2))
8
sbush$ echo $((1 ? 10 : 20))
10
sbush$ echo $((0 ? 10 : 20))
20
sbush$ echo $(( (5 > 3) ? 100 : 200 ))
100
```

**What to verify:**
1. `&` (bitwise AND): `3 & 1` → `1`
2. `|` (bitwise OR): `3 | 4` → `7`
3. `^` (bitwise XOR): `5 ^ 3` → `6`
4. `~` (bitwise NOT): `~0` → `-1`
5. `<<` (left shift): `1 << 4` → `16`
6. `>>` (right shift): `32 >> 2` → `8`
7. `? :` (ternary): `1 ? 10 : 20` → `10`, `0 ? 10 : 20` → `20`

### Step 6: Commit

```
ENH: Added bitwise and ternary operators to arithmetic evaluator
```
