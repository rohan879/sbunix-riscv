/*
 * stdlib.c — number conversions, sorting, and pseudo-random numbers
 *            for SBUnix libc.
 */

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

/* -----------------------------------------------------------------------
 * String-to-number conversions
 * ---------------------------------------------------------------------- */

/*
 * strtoul() — convert a string to an unsigned long.
 * Supports base 0 (auto-detect 0x/0 prefix), and bases 2-36.
 * Sets errno = ERANGE on overflow and returns ULONG_MAX.
 * POSIX: a leading '-' negates the result (wraps unsigned).
 */
unsigned long strtoul(const char *s, char **endptr, int base) {
    unsigned long result = 0;
    int neg = 0;
    int overflow = 0;

    /* Skip leading whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\r' || *s == '\f' || *s == '\v')
        s++;

    /* Optional sign — POSIX allows '-' for strtoul (negates result) */
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

    const unsigned long cutoff = ULONG_MAX / (unsigned long)base;
    const unsigned long cutlim = ULONG_MAX % (unsigned long)base;

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

        if (result > cutoff || (result == cutoff && (unsigned long)digit > cutlim))
            overflow = 1;

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

    /* Save errno; strtoul may set ERANGE for its own overflow */
    int saved_errno = errno;
    errno = 0;
    unsigned long val = strtoul(s, endptr, base);

    if (errno == ERANGE) {
        /* strtoul itself overflowed — clamp to LONG_MAX / LONG_MIN */
        return neg ? LONG_MIN : LONG_MAX;
    }

    if (neg) {
        /* The most negative value: -(unsigned long)LONG_MIN == LONG_MIN */
        if (val > (unsigned long)LONG_MAX + 1UL) {
            errno = ERANGE;
            return LONG_MIN;
        }
        errno = saved_errno;
        return -(long)val;
    } else {
        if (val > (unsigned long)LONG_MAX) {
            errno = ERANGE;
            return LONG_MAX;
        }
        errno = saved_errno;
        return (long)val;
    }
}

/*
 * strtoull() — convert a string to an unsigned long long.
 * Supports base 0 (auto-detect 0x/0 prefix), and bases 2-36.
 * Sets errno = ERANGE on overflow and returns ULLONG_MAX.
 * POSIX: a leading '-' negates the result (wraps unsigned).
 */
unsigned long long strtoull(const char *s, char **endptr, int base) {
    unsigned long long result = 0;
    int neg = 0;
    int overflow = 0;

    /* Skip leading whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\r' || *s == '\f' || *s == '\v')
        s++;

    /* Optional sign — POSIX allows '-' for strtoull (negates result) */
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

    const unsigned long long cutoff = ULLONG_MAX / (unsigned long long)base;
    const unsigned long long cutlim = ULLONG_MAX % (unsigned long long)base;

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

        if (result > cutoff || (result == cutoff && (unsigned long long)digit > cutlim))
            overflow = 1;

        result = result * base + digit;
        s++;
    }

    if (endptr)
        *endptr = (char *)(s == start ? start : s);

    if (overflow) {
        errno = ERANGE;
        return ULLONG_MAX;
    }

    return neg ? (unsigned long long)(-(long long)result) : result;
}

/*
 * strtoll() — convert a string to a long long.
 * Sets errno = ERANGE on overflow and returns LLONG_MAX or LLONG_MIN.
 */
long long strtoll(const char *s, char **endptr, int base) {
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

    /* Save errno; strtoull may set ERANGE for its own overflow */
    int saved_errno = errno;
    errno = 0;
    unsigned long long val = strtoull(s, endptr, base);

    if (errno == ERANGE) {
        /* strtoull itself overflowed — clamp to LLONG_MAX / LLONG_MIN */
        return neg ? LLONG_MIN : LLONG_MAX;
    }

    if (neg) {
        if (val > (unsigned long long)LLONG_MAX + 1ULL) {
            errno = ERANGE;
            return LLONG_MIN;
        }
        errno = saved_errno;
        return -(long long)val;
    } else {
        if (val > (unsigned long long)LLONG_MAX) {
            errno = ERANGE;
            return LLONG_MAX;
        }
        errno = saved_errno;
        return (long long)val;
    }
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
 * Integer division with quotient and remainder
 * ---------------------------------------------------------------------- */

div_t div(int numer, int denom) {
    div_t result;
    result.quot = numer / denom;
    result.rem  = numer % denom;
    return result;
}

ldiv_t ldiv(long numer, long denom) {
    ldiv_t result;
    result.quot = numer / denom;
    result.rem  = numer % denom;
    return result;
}

lldiv_t lldiv(long long numer, long long denom) {
    lldiv_t result;
    result.quot = numer / denom;
    result.rem  = numer % denom;
    return result;
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
    size_t vlen = value ? strlen(value) : 0;
    /* find existing */
    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=') {
            if (!overwrite) return 0;
            /* build new entry */
            char *buf = malloc(nlen + 1 + vlen + 1);
            if (!buf) return -1;
            strcpy(buf, name);
            strcat(buf, "=");
            if (value) strcat(buf, value);
            environ[i] = buf;
            return 0;
        }
    }
    /* add new */
    int count = 0;
    while (environ[count]) count++;
    if (count >= MAX_ENV) return -1;
    char *buf = malloc(nlen + 1 + vlen + 1);
    if (!buf) return -1;
    strcpy(buf, name);
    strcat(buf, "=");
    if (value) strcat(buf, value);
    environ[count] = buf;
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

/*
 * system() — execute a shell command.
 */
int system(const char *command) {
    if (!command) return 1; /* shell available */
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char *argv[4];
        argv[0] = "sh";
        argv[1] = "-c";
        argv[2] = (char *)command;
        argv[3] = (void *)0;
        execve("/bin/sh", argv, environ);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) ;
    return status;
}

/*
 * mkstemp() — create a temporary file with a unique name.
 */
int mkstemp(char *tmpl) {
    int len = strlen(tmpl);
    if (len < 6) return -1;
    char *suffix = tmpl + len - 6;
    static unsigned int counter = 0;
    for (int tries = 0; tries < 100; tries++) {
        unsigned int val = counter++ ^ (unsigned int)(long)tmpl;
        for (int i = 0; i < 6; i++) {
            int d = val % 36;
            suffix[i] = d < 10 ? ('0' + d) : ('a' + d - 10);
            val /= 36;
        }
        int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) return fd;
    }
    return -1;
}

int posix_openpt(int oflag) {
    return open("/dev/console", oflag);
}

int grantpt(int fd) {
    (void)fd;
    return 0;
}

int unlockpt(int fd) {
    (void)fd;
    return 0;
}

char *ptsname(int fd) {
    static char name[] = "/dev/console";
    if (fd < 0) {
        errno = EBADF;
        return (void *)0;
    }
    return name;
}

/*
 * remove() — remove a file or directory.
 */
int remove(const char *path) {
    if (unlink(path) == 0) return 0;
    return rmdir(path);
}

/*
 * realpath() — resolve a path to its canonical, absolute form.
 *
 * Resolves:
 *   - relative paths (prepends cwd)
 *   - "." and ".." components
 *   - symbolic links (via readlink)
 *
 * If resolved_path is NULL a static buffer is used.
 * Returns NULL on error (sets errno).
 */
char *realpath(const char *path, char *resolved_path) {
    static char static_buf[PATH_MAX];
    char work[PATH_MAX];   /* working copy we walk through */
    char result[PATH_MAX]; /* canonical result being built */
    int  rlen = 0;         /* length of result so far */
    int  symlinks = 0;     /* symlink recursion counter */

    if (!path || !*path) {
        errno = EINVAL;
        return (void *)0;
    }

    if (!resolved_path)
        resolved_path = static_buf;

    /* Build an absolute working path */
    if (path[0] != '/') {
        if (!getcwd(work, sizeof(work)))
            return (void *)0;
        int wlen = strlen(work);
        if (wlen + 1 + (int)strlen(path) + 1 > (int)sizeof(work)) {
            errno = ENAMETOOLONG;
            return (void *)0;
        }
        work[wlen] = '/';
        strcpy(work + wlen + 1, path);
    } else {
        if ((int)strlen(path) >= (int)sizeof(work)) {
            errno = ENAMETOOLONG;
            return (void *)0;
        }
        strcpy(work, path);
    }

    /* result starts as "/" */
    result[0] = '/';
    result[1] = '\0';
    rlen = 1;

    char *p = work;
    while (*p) {
        /* Skip slashes */
        while (*p == '/')
            p++;
        if (!*p)
            break;

        /* Extract the next component */
        char *seg = p;
        while (*p && *p != '/')
            p++;
        int slen = (int)(p - seg);

        /* "." — skip */
        if (slen == 1 && seg[0] == '.')
            continue;

        /* ".." — go up one directory */
        if (slen == 2 && seg[0] == '.' && seg[1] == '.') {
            /* Strip trailing component from result */
            if (rlen > 1) {
                rlen--; /* step back over trailing char */
                while (rlen > 1 && result[rlen - 1] != '/')
                    rlen--;
            }
            result[rlen] = '\0';
            continue;
        }

        /* Append '/' if result doesn't already end with one */
        if (rlen > 0 && result[rlen - 1] != '/') {
            if (rlen + 1 >= (int)sizeof(result)) {
                errno = ENAMETOOLONG;
                return (void *)0;
            }
            result[rlen++] = '/';
            result[rlen] = '\0';
        }

        /* Append the component */
        if (rlen + slen >= (int)sizeof(result)) {
            errno = ENAMETOOLONG;
            return (void *)0;
        }
        memcpy(result + rlen, seg, slen);
        rlen += slen;
        result[rlen] = '\0';

        /* Check if this path is a symlink */
        char link_buf[PATH_MAX];
        ssize_t llen = readlink(result, link_buf, sizeof(link_buf) - 1);
        if (llen >= 0) {
            /* It is a symlink */
            if (++symlinks > 40) {
                errno = ELOOP;
                return (void *)0;
            }
            link_buf[llen] = '\0';

            /* Build new working path: resolved_link + remaining */
            char newwork[PATH_MAX];
            int remaining_len = (int)strlen(p);

            if (link_buf[0] == '/') {
                /* Absolute symlink: restart from root */
                if (llen + 1 + remaining_len + 1 > (int)sizeof(newwork)) {
                    errno = ENAMETOOLONG;
                    return (void *)0;
                }
                memcpy(newwork, link_buf, llen);
                if (remaining_len > 0) {
                    newwork[llen] = '/';
                    memcpy(newwork + llen + 1, p, remaining_len + 1);
                } else {
                    newwork[llen] = '\0';
                }
                /* Reset result to root */
                result[0] = '/';
                result[1] = '\0';
                rlen = 1;
            } else {
                /* Relative symlink: resolve relative to parent dir */
                /* Strip the component we just appended from result */
                if (rlen > 1) {
                    rlen -= slen;
                    result[rlen] = '\0';
                }
                /* Remove trailing '/' if result isn't just "/" */
                if (rlen > 1 && result[rlen - 1] == '/') {
                    rlen--;
                    result[rlen] = '\0';
                }

                int parent_rlen = rlen; /* save for absolute prefix later */
                (void)parent_rlen;

                if (llen + 1 + remaining_len + 1 > (int)sizeof(newwork)) {
                    errno = ENAMETOOLONG;
                    return (void *)0;
                }
                memcpy(newwork, link_buf, llen);
                if (remaining_len > 0) {
                    newwork[llen] = '/';
                    memcpy(newwork + llen + 1, p, remaining_len + 1);
                } else {
                    newwork[llen] = '\0';
                }
            }

            /* Continue parsing from newwork */
            if ((int)strlen(newwork) >= (int)sizeof(work)) {
                errno = ENAMETOOLONG;
                return (void *)0;
            }
            strcpy(work, newwork);
            p = work;
        }
        /* Not a symlink — continue with next component */
    }

    /* Remove trailing '/' unless the result is just "/" */
    if (rlen > 1 && result[rlen - 1] == '/') {
        rlen--;
        result[rlen] = '\0';
    }

    if (rlen >= (int)sizeof(result)) {
        errno = ENAMETOOLONG;
        return (void *)0;
    }

    memcpy(resolved_path, result, rlen + 1);
    return resolved_path;
}
