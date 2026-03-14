/*
 * stdlib.c — number conversions, sorting, and pseudo-random numbers
 *            for SBUnix libc.
 */

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

/* -----------------------------------------------------------------------
 * String-to-number conversions
 * ---------------------------------------------------------------------- */

/*
 * strtoul() — convert a string to an unsigned long.
 * Supports base 0 (auto-detect 0x/0 prefix), and bases 2–36.
 */
unsigned long strtoul(const char *s, char **endptr, int base) {
    unsigned long result = 0;
    int neg = 0;
    int overflow = 0;

    /* Skip leading whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\r' || *s == '\f' || *s == '\v')
        s++;

    /* Optional sign */
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }

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

    unsigned long cutoff = ULONG_MAX / (unsigned long)base;
    int cutlim = (int)(ULONG_MAX % (unsigned long)base);

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

        if (result > cutoff || (result == cutoff && digit > cutlim)) {
            overflow = 1;
        }
        result = result * base + digit;
        s++;
    }

    if (endptr)
        *endptr = (char *)(s == start ? start : s);

    if (overflow) {
        errno = ERANGE;
        return ULONG_MAX;
    }

    return neg ? (unsigned long)(-(long)result) : result;
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

    /* Use strtoul for digit parsing but suppress its errno so we can
     * apply signed range checks ourselves */
    int saved_errno = errno;
    errno = 0;
    unsigned long val = strtoul(s, endptr, base);
    if (errno == ERANGE) {
        /* strtoul already overflowed unsigned range */
        return neg ? LONG_MIN : LONG_MAX;
    }
    errno = saved_errno;

    if (neg) {
        if (val > (unsigned long)LONG_MAX + 1UL) {
            errno = ERANGE;
            return LONG_MIN;
        }
        return -(long)val;
    } else {
        if (val > (unsigned long)LONG_MAX) {
            errno = ERANGE;
            return LONG_MAX;
        }
        return (long)val;
    }
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

    /* Rebuild path with length tracking to prevent overflow */
    out[0] = '/';
    out[1] = '\0';
    size_t outlen = 1;
    for (int i = 0; i < ncomp; i++) {
        size_t clen = strlen(components[i]);
        size_t need = (i > 0 ? 1 : 0) + clen; /* separator + component */
        if (outlen + need >= PATH_MAX) {
            errno = 36; /* ENAMETOOLONG */
            return (void *)0;
        }
        if (i > 0) {
            out[outlen++] = '/';
        } else {
            outlen = 1; /* overwrite NUL after initial / */
        }
        memcpy(out + outlen, components[i], clen);
        outlen += clen;
        out[outlen] = '\0';
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

/* -----------------------------------------------------------------------
 * Temporary file creation: mkstemp / mktemp
 * ---------------------------------------------------------------------- */

#include <fcntl.h>
#include <errno.h>

/* Forward declarations to avoid pulling in all of syscall.h (which
 * conflicts with sys/wait.h macros included later in this file). */
#ifndef SYS_getrandom
#define SYS_getrandom 278
#endif
long syscall(long num, ...);

/*
 * _mktemp_fill() — fill trailing XXXXXX in template with pseudo-random
 * characters.  Uses SYS_getrandom if available, falls back to a mix of
 * getpid() and a counter.
 */
static int _mktemp_fill(char *tmpl) {
    static const char chars[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    int len = strlen(tmpl);
    if (len < 6) {
        errno = EINVAL;
        return -1;
    }
    /* Verify trailing XXXXXX */
    for (int i = len - 6; i < len; i++) {
        if (tmpl[i] != 'X') {
            errno = EINVAL;
            return -1;
        }
    }

    /* Try to get randomness from the kernel */
    unsigned char randbuf[6];
    long ret = syscall(SYS_getrandom, randbuf, 6, 0);
    if (ret == 6) {
        for (int i = 0; i < 6; i++)
            tmpl[len - 6 + i] = chars[randbuf[i] % 62];
    } else {
        /* Fallback: mix pid and a counter */
        static unsigned int counter = 0;
        unsigned int seed = (unsigned int)getpid() * 31u + counter++;
        for (int i = 0; i < 6; i++) {
            seed = seed * 1103515245u + 12345u;
            tmpl[len - 6 + i] = chars[(seed >> 16) % 62];
        }
    }
    return 0;
}

/*
 * mkstemp() — create and open a unique temporary file.
 * template must end with "XXXXXX".  Returns an open fd or -1.
 */
int mkstemp(char *tmpl) {
    for (int attempts = 0; attempts < 100; attempts++) {
        /* Save the template suffix in case we need to retry */
        char saved[7];
        int len = strlen(tmpl);
        if (len < 6) { errno = EINVAL; return -1; }
        memcpy(saved, tmpl + len - 6, 6);
        saved[6] = '\0';

        if (_mktemp_fill(tmpl) < 0)
            return -1;

        int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0)
            return fd;

        if (errno != EEXIST)
            return -1;

        /* Restore XXXXXX for next attempt */
        memcpy(tmpl + len - 6, saved, 6);
    }

    errno = EEXIST;
    return -1;
}

/*
 * mktemp() — generate a unique temporary filename without opening it.
 * template must end with "XXXXXX".  Returns template or empty string on error.
 */
char *mktemp(char *tmpl) {
    if (_mktemp_fill(tmpl) < 0) {
        tmpl[0] = '\0';
        return tmpl;
    }
    return tmpl;
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
