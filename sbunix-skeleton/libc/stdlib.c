/*
 * stdlib.c — number conversions, sorting, and pseudo-random numbers
 *            for SBUnix libc.
 */

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

/* -----------------------------------------------------------------------
 * String-to-number conversions
 * ---------------------------------------------------------------------- */

/*
 * strtoul() — convert a string to an unsigned long.
 * Supports base 0 (auto-detect 0x/0 prefix), and bases 2–36.
 */
unsigned long strtoul(const char *s, char **endptr, int base) {
    unsigned long result = 0;

    /* Skip leading whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\r' || *s == '\f' || *s == '\v')
        s++;

    /* Optional sign (unsigned: '+' only) */
    if (*s == '+')
        s++;

    /* Auto-detect base */
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            s += 2;
        } else if (s[0] == '0') {
            base = 8;
            s++;
        } else {
            base = 10;
        }
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    const char *start = s;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9')
            digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z')
            digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z')
            digit = *s - 'A' + 10;
        else
            break;

        if (digit >= base)
            break;

        result = result * base + digit;
        s++;
    }

    if (endptr)
        *endptr = (char *)(s == start ? start : s);

    return result;
}

long strtol(const char *s, char **endptr, int base) {
    /* Skip leading whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\r' || *s == '\f' || *s == '\v')
        s++;

    int neg = 0;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    unsigned long val = strtoul(s, endptr, base);
    return neg ? -(long)val : (long)val;
}

/* On LP64 (rv64), long and long long are both 64-bit. */
long long strtoll(const char *s, char **endptr, int base) {
    return (long long)strtol(s, endptr, base);
}

unsigned long long strtoull(const char *s, char **endptr, int base) {
    return (unsigned long long)strtoul(s, endptr, base);
}

int atoi(const char *s) {
    return (int)strtol(s, (char **)0, 10);
}

long atol(const char *s) {
    return strtol(s, (char **)0, 10);
}

/*
 * itoa() — convert integer to string in the given base.
 * buf must be large enough (e.g. 33 bytes for base-2 + sign + NUL).
 * Returns buf.
 */
char *itoa(int value, char *buf, int base) {
    if (base < 2 || base > 36) {
        buf[0] = '\0';
        return buf;
    }

    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[34];
    int  i = 0;
    int  neg = 0;

    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return buf;
    }

    if (value < 0 && base == 10) {
        neg = 1;
        /* Avoid undefined behaviour for INT_MIN */
        unsigned int uval = (unsigned int)(-(value + 1)) + 1u;
        while (uval) {
            tmp[i++] = digits[uval % base];
            uval /= base;
        }
    } else {
        unsigned int uval = (unsigned int)value;
        while (uval) {
            tmp[i++] = digits[uval % base];
            uval /= base;
        }
    }

    int j = 0;
    if (neg)
        buf[j++] = '-';
    while (i--)
        buf[j++] = tmp[i];
    buf[j] = '\0';
    return buf;
}

/* -----------------------------------------------------------------------
 * Pseudo-random number generator (simple LCG)
 * ---------------------------------------------------------------------- */

static unsigned int rand_state = 1;

int rand(void) {
    rand_state = rand_state * 1103515245u + 12345u;
    return (int)((rand_state >> 16) & 0x7fff);
}

void srand(unsigned int seed) {
    rand_state = seed;
}

/* -----------------------------------------------------------------------
 * Absolute value
 * ---------------------------------------------------------------------- */

int abs(int x) {
    return x < 0 ? -x : x;
}

long labs(long x) {
    return x < 0 ? -x : x;
}

/* -----------------------------------------------------------------------
 * Sorting: qsort (iterative quicksort, in-place)
 * ---------------------------------------------------------------------- */

static void swap_bytes(char *a, char *b, size_t size) {
    while (size--) {
        char tmp = *a;
        *a++ = *b;
        *b++ = tmp;
    }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (nmemb < 2 || size == 0)
        return;

    /* Iterative quicksort using an explicit stack */
    /* Maximum stack depth for 2^64 elements is 64 */
    long lo_stack[64], hi_stack[64];
    int  top = 0;

    lo_stack[top] = 0;
    hi_stack[top] = (long)nmemb - 1;
    top++;

    while (top > 0) {
        top--;
        long lo = lo_stack[top];
        long hi = hi_stack[top];

        if (lo >= hi)
            continue;

        /* Median-of-three pivot selection */
        long mid = lo + (hi - lo) / 2;
        char *b = (char *)base;

        if (compar(b + lo * size, b + mid * size) > 0)
            swap_bytes(b + lo * size, b + mid * size, size);
        if (compar(b + lo * size, b + hi * size) > 0)
            swap_bytes(b + lo * size, b + hi * size, size);
        if (compar(b + mid * size, b + hi * size) > 0)
            swap_bytes(b + mid * size, b + hi * size, size);

        /* Pivot is now at mid; move it to hi-1 */
        swap_bytes(b + mid * size, b + (hi - 1) * size, size);
        char *pivot = b + (hi - 1) * size;

        long i = lo, j = hi - 1;
        while (1) {
            do { i++; } while (i < hi  && compar(b + i * size, pivot) < 0);
            do { j--; } while (j > lo  && compar(b + j * size, pivot) > 0);
            if (i >= j)
                break;
            swap_bytes(b + i * size, b + j * size, size);
        }
        swap_bytes(b + i * size, b + (hi - 1) * size, size);

        /* Push sub-partitions */
        if (top < 63) {
            lo_stack[top] = lo;
            hi_stack[top] = i - 1;
            top++;
            lo_stack[top] = i + 1;
            hi_stack[top] = hi;
            top++;
        }
    }
}

/*
 * bsearch() — binary search in a sorted array.
 */
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    const char *lo = (const char *)base;
    size_t      count = nmemb;

    while (count > 0) {
        size_t      half = count / 2;
        const char *mid  = lo + half * size;
        int cmp = compar(key, mid);
        if (cmp == 0)
            return (void *)mid;
        if (cmp > 0) {
            lo    = mid + size;
            count = count - half - 1;
        } else {
            count = half;
        }
    }
    return (void *)0;
}

/* -----------------------------------------------------------------------
 * realpath() — resolve a pathname to an absolute path.
 * Handles . and .. components.  Does not follow symlinks beyond what
 * the kernel already resolves on stat().
 * ---------------------------------------------------------------------- */

#include <sys/stat.h>
#include <limits.h>
#include <errno.h>

char *realpath(const char *path, char *resolved_path) {
    if (!path || !*path) {
        errno = 22; /* EINVAL */
        return (void *)0;
    }

    static char buf[PATH_MAX];
    char *out = resolved_path ? resolved_path : buf;
    char work[PATH_MAX];

    /* Start with absolute path */
    if (path[0] != '/') {
        if (!getcwd(out, PATH_MAX))
            return (void *)0;
        size_t cwdlen = strlen(out);
        if (cwdlen + 1 + strlen(path) >= PATH_MAX) {
            errno = 36; /* ENAMETOOLONG */
            return (void *)0;
        }
        out[cwdlen] = '/';
        strcpy(out + cwdlen + 1, path);
    } else {
        if (strlen(path) >= PATH_MAX) {
            errno = 36;
            return (void *)0;
        }
        strcpy(out, path);
    }

    /* Normalize: resolve . and .. */
    strcpy(work, out);
    char *components[256];
    int ncomp = 0;

    char *p = work;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = '\0';

        if (strcmp(start, ".") == 0) {
            continue;
        } else if (strcmp(start, "..") == 0) {
            if (ncomp > 0) ncomp--;
        } else {
            if (ncomp < 256)
                components[ncomp++] = start;
        }
    }

    /* Rebuild path */
    out[0] = '/';
    out[1] = '\0';
    for (int i = 0; i < ncomp; i++) {
        if (i > 0) strcat(out, "/");
        else out[1] = '\0'; /* overwrite the initial NUL after / */
        if (i == 0)
            strcpy(out + 1, components[i]);
        else {
            strcat(out, components[i]);
        }
    }

    /* Verify the path exists */
    struct stat st;
    if (stat(out, &st) < 0)
        return (void *)0;

    return out;
}

/* -----------------------------------------------------------------------
 * system() — execute a command via the shell.
 * ---------------------------------------------------------------------- */

#include <sys/wait.h>

int system(const char *command) {
    if (!command)
        return 1; /* shell is available */

    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        /* child */
        char *argv[4];
        argv[0] = "sh";
        argv[1] = "-c";
        argv[2] = (char *)command;
        argv[3] = (void *)0;
        execve("/bin/sh", argv, environ);
        _exit(127);
    }

    /* parent */
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return status;
}

/*
 * abort() — terminate the process abnormally.
 */
void abort(void) {
    raise(SIGABRT);
    _exit(134);
}

/*
 * getenv() — look up an environment variable.
 * Scans the global environ array.
 */
extern char **environ;

char *getenv(const char *name) {
    if (!environ || !name)
        return (void *)0;
    size_t len = strlen(name);
    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], name, len) == 0 && environ[i][len] == '=')
            return environ[i] + len + 1;
    }
    return (void *)0;
}

/*
 * Environment manipulation.
 * These operate on the global environ array.
 * Simple implementation: maintains a static array of "NAME=VALUE" strings.
 */

#define MAX_ENV 128
static char *env_storage[MAX_ENV + 1];
static int env_initialized = 0;

static void env_ensure_init(void) {
    if (env_initialized) return;
    env_initialized = 1;
    int i = 0;
    if (environ) {
        for (; environ[i] && i < MAX_ENV; i++)
            env_storage[i] = environ[i];
    }
    env_storage[i] = (void *)0;
    environ = env_storage;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '='))
        return -1;
    env_ensure_init();
    size_t nlen = strlen(name);
    size_t vlen = strlen(value);

    /* Allocate a new "NAME=VALUE" string */
    char *entry = (char *)malloc(nlen + 1 + vlen + 1);
    if (!entry)
        return -1;
    memcpy(entry, name, nlen);
    entry[nlen] = '=';
    memcpy(entry + nlen + 1, value, vlen);
    entry[nlen + 1 + vlen] = '\0';

    /* find existing */
    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=') {
            if (!overwrite) {
                free(entry);
                return 0;
            }
            environ[i] = entry;
            return 0;
        }
    }
    /* add new */
    int count = 0;
    while (environ[count]) count++;
    if (count >= MAX_ENV) {
        free(entry);
        return -1;
    }
    environ[count] = entry;
    environ[count + 1] = (void *)0;
    return 0;
}

int unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '='))
        return -1;
    env_ensure_init();
    size_t nlen = strlen(name);
    int i = 0;
    while (environ[i]) {
        if (strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=') {
            /* shift remaining entries down */
            int j = i;
            while (environ[j]) { environ[j] = environ[j + 1]; j++; }
        } else {
            i++;
        }
    }
    return 0;
}

int putenv(char *string) {
    if (!string) return -1;
    env_ensure_init();
    char *eq = strchr(string, '=');
    if (!eq) return unsetenv(string);
    size_t nlen = (size_t)(eq - string);
    /* replace existing */
    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], string, nlen) == 0 && environ[i][nlen] == '=') {
            environ[i] = string;
            return 0;
        }
    }
    /* add new */
    int count = 0;
    while (environ[count]) count++;
    if (count >= MAX_ENV) return -1;
    environ[count] = string;
    environ[count + 1] = (void *)0;
    return 0;
}
