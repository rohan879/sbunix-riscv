#include "sbush.h"

static char var_names[MAX_VARS][64];
static char var_values[MAX_VARS][256];
static int  var_exported[MAX_VARS];
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
    nvar++;
}

void var_unset(const char *name) {
    int i = find_var(name);
    if (i < 0)
        return;
    /* swap-remove: move last entry into this slot */
    nvar--;
    if (i < nvar) {
        strcpy(var_names[i], var_names[nvar]);
        strcpy(var_values[i], var_values[nvar]);
        var_exported[i] = var_exported[nvar];
    }
    var_names[nvar][0] = '\0';
    var_values[nvar][0] = '\0';
    var_exported[nvar] = 0;
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
