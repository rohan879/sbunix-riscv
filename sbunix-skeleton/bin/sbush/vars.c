#include "sbush.h"

static char var_names[MAX_VARS][64];
static char var_values[MAX_VARS][256];
static int  var_exported[MAX_VARS];
static int  var_readonly[MAX_VARS];
static int  nvar;

static char *env_ptrs[MAX_VARS + 1];
static char  env_bufs[MAX_VARS][320];

static int find_var(const char *name) {
    for (int i = 0; i < nvar; i++)
        if (strcmp(var_names[i], name) == 0)
            return i;
    return -1;
}

void env_init(char **envp) {
    if (!envp)
        return;
    for (int i = 0; envp[i] && nvar < MAX_VARS; i++) {
        char *eq = strchr(envp[i], '=');
        if (!eq)
            continue;
        int nlen = eq - envp[i];
        if (nlen >= 64)
            nlen = 63;
        strncpy(var_names[nvar], envp[i], nlen);
        var_names[nvar][nlen] = '\0';
        strncpy(var_values[nvar], eq + 1, 255);
        var_values[nvar][255] = '\0';
        var_exported[nvar] = 1;
        nvar++;
    }
}

char *var_get(const char *name) {
    int i = find_var(name);
    if (i >= 0)
        return var_values[i];
    return NULL;
}

void var_set(const char *name, const char *value) {
    int i = find_var(name);
    if (i >= 0) {
        if (var_readonly[i]) {
            dprintf(2, "sbush: %s: readonly variable\n", name);
            return;
        }
        strncpy(var_values[i], value, 255);
        var_values[i][255] = '\0';
        return;
    }
    if (nvar >= MAX_VARS)
        return;
    strncpy(var_names[nvar], name, 63);
    var_names[nvar][63] = '\0';
    strncpy(var_values[nvar], value, 255);
    var_values[nvar][255] = '\0';
    var_exported[nvar] = 0;
    var_readonly[nvar] = 0;
    nvar++;
}

int var_set_checked(const char *name, const char *value) {
    int i = find_var(name);
    if (i >= 0 && var_readonly[i]) {
        dprintf(2, "sbush: %s: readonly variable\n", name);
        return -1;
    }
    var_set(name, value);
    return 0;
}

void var_unset(const char *name) {
    int i = find_var(name);
    if (i < 0)
        return;
    if (var_readonly[i]) {
        dprintf(2, "sbush: unset: %s: readonly variable\n", name);
        return;
    }
    /* swap-remove: move last entry into this slot */
    nvar--;
    if (i < nvar) {
        strcpy(var_names[i], var_names[nvar]);
        strcpy(var_values[i], var_values[nvar]);
        var_exported[i] = var_exported[nvar];
        var_readonly[i] = var_readonly[nvar];
    }
    var_names[nvar][0] = '\0';
    var_values[nvar][0] = '\0';
    var_exported[nvar] = 0;
    var_readonly[nvar] = 0;
}

int var_unset_checked(const char *name) {
    int i = find_var(name);
    if (i >= 0 && var_readonly[i]) {
        dprintf(2, "sbush: unset: %s: readonly variable\n", name);
        return -1;
    }
    var_unset(name);
    return 0;
}

void var_set_readonly(const char *name) {
    int i = find_var(name);
    if (i >= 0)
        var_readonly[i] = 1;
}

int var_is_readonly(const char *name) {
    int i = find_var(name);
    if (i >= 0)
        return var_readonly[i];
    return 0;
}

void var_list_readonly(void) {
    for (int i = 0; i < nvar; i++) {
        if (var_readonly[i])
            dprintf(STDOUT_FILENO, "readonly %s='%s'\n", var_names[i], var_values[i]);
    }
}

void var_export(const char *name) {
    int i = find_var(name);
    if (i >= 0)
        var_exported[i] = 1;
}

char **build_environ(void) {
    int j = 0;
    for (int i = 0; i < nvar && j < MAX_VARS; i++) {
        if (!var_exported[i])
            continue;
        strcpy(env_bufs[j], var_names[i]);
        strcat(env_bufs[j], "=");
        strcat(env_bufs[j], var_values[i]);
        env_ptrs[j] = env_bufs[j];
        j++;
    }
    env_ptrs[j] = NULL;
    return env_ptrs;
}

/* -----------------------------------------------------------------------
 * Positional parameters ($0, $1...$N, $#, $@, $*)
 * ---------------------------------------------------------------------- */
static char pos_params[MAX_POSITIONAL][256];
static char pos_zero[256];
int positional_count = 0;

void positional_set_zero(const char *name) {
    strncpy(pos_zero, name, 255);
    pos_zero[255] = '\0';
}

char *positional_get(int idx) {
    if (idx == 0)
        return pos_zero;
    if (idx < 1 || idx > positional_count)
        return NULL;
    return pos_params[idx - 1];
}

void positional_set_args(int argc, char **argv) {
    positional_count = 0;
    for (int i = 0; i < argc && i < MAX_POSITIONAL; i++) {
        strncpy(pos_params[i], argv[i], 255);
        pos_params[i][255] = '\0';
        positional_count++;
    }
}

void positional_shift(int n) {
    if (n <= 0 || n > positional_count) return;
    for (int i = 0; i < positional_count - n; i++) {
        strcpy(pos_params[i], pos_params[i + n]);
    }
    for (int i = positional_count - n; i < positional_count; i++) {
        pos_params[i][0] = '\0';
    }
    positional_count -= n;
}

/* -----------------------------------------------------------------------
 * Local variable scoping for functions
 * Save/restore variables declared as 'local' within a function body.
 * ---------------------------------------------------------------------- */
#define LOCAL_SCOPE_DEPTH 4

struct local_scope {
    int  count;
    char names[MAX_LOCAL_VARS][64];
    char values[MAX_LOCAL_VARS][256];
    int  existed[MAX_LOCAL_VARS]; /* 1 if variable existed before local */
};

static struct local_scope local_scopes[LOCAL_SCOPE_DEPTH];
static int local_depth = 0;

void local_scope_push(void) {
    if (local_depth >= LOCAL_SCOPE_DEPTH) return;
    local_scopes[local_depth].count = 0;
    local_depth++;
}

void local_scope_pop(void) {
    if (local_depth <= 0) return;
    local_depth--;
    struct local_scope *sc = &local_scopes[local_depth];
    /* Restore saved values in reverse order */
    for (int i = sc->count - 1; i >= 0; i--) {
        if (sc->existed[i]) {
            /* Bypass readonly check for restore — force direct set */
            int idx = find_var(sc->names[i]);
            if (idx >= 0) {
                strncpy(var_values[idx], sc->values[i], 255);
                var_values[idx][255] = '\0';
            } else {
                var_set(sc->names[i], sc->values[i]);
            }
        } else {
            /* Variable did not exist before; remove it */
            int idx = find_var(sc->names[i]);
            if (idx >= 0) {
                nvar--;
                if (idx < nvar) {
                    strcpy(var_names[idx], var_names[nvar]);
                    strcpy(var_values[idx], var_values[nvar]);
                    var_exported[idx] = var_exported[nvar];
                    var_readonly[idx] = var_readonly[nvar];
                }
                var_names[nvar][0] = '\0';
                var_values[nvar][0] = '\0';
                var_exported[nvar] = 0;
                var_readonly[nvar] = 0;
            }
        }
    }
}

void local_declare(const char *name) {
    if (local_depth <= 0) {
        dprintf(2, "sbush: local: can only be used in a function\n");
        return;
    }
    struct local_scope *sc = &local_scopes[local_depth - 1];
    if (sc->count >= MAX_LOCAL_VARS) return;

    int slot = sc->count++;
    strncpy(sc->names[slot], name, 63);
    sc->names[slot][63] = '\0';

    char *existing = var_get(name);
    if (existing) {
        sc->existed[slot] = 1;
        strncpy(sc->values[slot], existing, 255);
        sc->values[slot][255] = '\0';
    } else {
        sc->existed[slot] = 0;
        sc->values[slot][0] = '\0';
    }
}
