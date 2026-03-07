# Libc Header & Implementation Completeness Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add missing POSIX headers, declarations, and function implementations to pass grading tests that expect standard include paths and function availability.

**Architecture:** Pure userspace changes — no kernel modifications. Add 3 missing `sys/` headers, add missing declarations to 4 existing headers, implement 15+ missing functions across 3 `.c` files.

**Tech Stack:** C (freestanding, `riscv64-unknown-elf-gcc`, `-ffreestanding -nostdlib -nostdinc`), libc headers at `libc/include/`, implementations at `libc/`.

---

## Task 1: Create `sys/time.h`

**Files:**
- Create: `libc/include/sys/time.h`

Tests expect `#include <sys/time.h>` to provide `struct timeval`, `gettimeofday()`, and timer arithmetic macros. The root `time.h` already defines `struct timeval`, so this header includes it and adds what's missing.

**Step 1: Create the header**

```c
#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <time.h>      /* struct timeval, struct timespec */
#include <sys/types.h>

/* gettimeofday() */
int gettimeofday(struct timeval *tv, void *tz);

/* Timer arithmetic macros */
#define timersub(a, b, result) \
  do { \
    (result)->tv_sec  = (a)->tv_sec  - (b)->tv_sec; \
    (result)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
    if ((result)->tv_usec < 0) { \
      --(result)->tv_sec; \
      (result)->tv_usec += 1000000; \
    } \
  } while (0)

#define timeradd(a, b, result) \
  do { \
    (result)->tv_sec  = (a)->tv_sec  + (b)->tv_sec; \
    (result)->tv_usec = (a)->tv_usec + (b)->tv_usec; \
    if ((result)->tv_usec >= 1000000) { \
      ++(result)->tv_sec; \
      (result)->tv_usec -= 1000000; \
    } \
  } while (0)

#define timercmp(a, b, CMP) \
  (((a)->tv_sec == (b)->tv_sec) \
    ? ((a)->tv_usec CMP (b)->tv_usec) \
    : ((a)->tv_sec CMP (b)->tv_sec))

#define timerclear(tvp) ((tvp)->tv_sec = (tvp)->tv_usec = 0)
#define timerisset(tvp) ((tvp)->tv_sec || (tvp)->tv_usec)

#endif /* _SYS_TIME_H */
```

**Step 2: Implement `gettimeofday()` in `libc/time.c`**

Add at end of `libc/time.c`:

```c
int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv)
        return -1;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
        return -1;
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}
```

Also add the declaration to the root `libc/include/time.h` before the `#endif`:

```c
int gettimeofday(struct timeval *tv, void *tz);
```

**Step 3: Build and verify**

Run: `cd sbunix-skeleton && source tools/sourceme.sh && make clean && make`
Expected: Compiles with no errors or warnings.

**Step 4: Commit**

```bash
git add libc/include/sys/time.h libc/include/time.h libc/time.c
git commit -m "ENH: Added sys/time.h with gettimeofday and timer macros"
```

---

## Task 2: Create `sys/syscall.h`

**Files:**
- Create: `libc/include/sys/syscall.h`

Tests expect `#include <sys/syscall.h>` to provide syscall numbers.

**Step 1: Create the header**

```c
#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H

/* Re-export all syscall numbers from the main header */
#include <syscall.h>

/* POSIX aliases some tests may use */
#ifndef __NR_write
#define __NR_write       SYS_write
#define __NR_read        SYS_read
#define __NR_openat      SYS_openat
#define __NR_close       SYS_close
#define __NR_exit        SYS_exit
#define __NR_clone       SYS_clone
#define __NR_execve      SYS_execve
#define __NR_getpid      SYS_getpid
#define __NR_kill        SYS_kill
#define __NR_mmap        SYS_mmap
#define __NR_munmap      SYS_munmap
#define __NR_brk         SYS_brk
#endif

#endif /* _SYS_SYSCALL_H */
```

**Step 2: Build and verify**

Run: `make clean && make`
Expected: No errors.

**Step 3: Commit**

```bash
git add libc/include/sys/syscall.h
git commit -m "ENH: Added sys/syscall.h with syscall number re-exports"
```

---

## Task 3: Create `sys/ioctl.h`

**Files:**
- Create: `libc/include/sys/ioctl.h`

Needed for `isatty()` and terminal size queries.

**Step 1: Create the header**

```c
#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#include <sys/types.h>

/* Terminal window size */
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

/* ioctl request codes */
#define TIOCGWINSZ  0x5413  /* get window size */
#define TIOCSWINSZ  0x5414  /* set window size */
#define TIOCGPGRP   0x540F  /* get foreground process group */
#define TIOCSPGRP   0x5410  /* set foreground process group */
#define FIONREAD    0x541B  /* bytes available to read */

int ioctl(int fd, unsigned long request, ...);

#endif /* _SYS_IOCTL_H */
```

**Step 2: Add `ioctl()` wrapper to `libc/unistd.c`**

Add at end of `libc/unistd.c`:

```c
int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return (int)syscall(SYS_ioctl, fd, request, arg);
}
```

Also add `#include <sys/ioctl.h>` to the includes at top of `libc/unistd.c`.

**Step 3: Build and verify**

Run: `make clean && make`
Expected: No errors.

**Step 4: Commit**

```bash
git add libc/include/sys/ioctl.h libc/unistd.c
git commit -m "ENH: Added sys/ioctl.h with ioctl wrapper and terminal ioctls"
```

---

## Task 4: Add missing string declarations and implementations

**Files:**
- Modify: `libc/include/string.h:18` (before `#endif`)
- Modify: `libc/string.c:119` (append after `memcmp`)

**Step 1: Add declarations to `string.h`**

Insert before the `#endif` on line 20:

```c
void  *memmove(void *dst, const void *src, size_t n);
void  *memchr(const void *s, int c, size_t n);
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);
char  *strncat(char *dst, const char *src, size_t n);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strpbrk(const char *s, const char *accept);
char  *strtok(char *str, const char *delim);
char  *strerror(int errnum);
```

**Step 2: Add implementations to `string.c`**

Append after the existing `memcmp` function:

```c
void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    while (n--) {
        if (*p == (unsigned char)c)
            return (void *)p;
        p++;
    }
    return NULL;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *d = malloc(len);
    if (d)
        memcpy(d, s, len);
    return d;
}

char *strndup(const char *s, size_t n) {
    size_t len = strlen(s);
    if (len > n)
        len = n;
    char *d = malloc(len + 1);
    if (d) {
        memcpy(d, s, len);
        d[len] = '\0';
    }
    return d;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (*d)
        d++;
    while (n-- && *src)
        *d++ = *src++;
    *d = '\0';
    return dst;
}

size_t strspn(const char *s, const char *accept) {
    size_t count = 0;
    while (*s) {
        const char *a = accept;
        int found = 0;
        while (*a) {
            if (*s == *a) { found = 1; break; }
            a++;
        }
        if (!found) break;
        count++;
        s++;
    }
    return count;
}

size_t strcspn(const char *s, const char *reject) {
    size_t count = 0;
    while (*s) {
        const char *r = reject;
        while (*r) {
            if (*s == *r) return count;
            r++;
        }
        count++;
        s++;
    }
    return count;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        const char *a = accept;
        while (*a) {
            if (*s == *a) return (char *)s;
            a++;
        }
        s++;
    }
    return NULL;
}

static char *strtok_saveptr;

char *strtok(char *str, const char *delim) {
    if (str)
        strtok_saveptr = str;
    if (!strtok_saveptr)
        return NULL;

    /* skip leading delimiters */
    char *start = strtok_saveptr;
    while (*start && strchr(delim, *start))
        start++;
    if (!*start) {
        strtok_saveptr = NULL;
        return NULL;
    }

    /* find end of token */
    char *end = start;
    while (*end && !strchr(delim, *end))
        end++;
    if (*end) {
        *end = '\0';
        strtok_saveptr = end + 1;
    } else {
        strtok_saveptr = NULL;
    }
    return start;
}

char *strerror(int errnum) {
    static char buf[32];
    char *p = buf;
    char *prefix = "error ";
    while (*prefix)
        *p++ = *prefix++;
    /* simple int-to-string */
    if (errnum < 0) { *p++ = '-'; errnum = -errnum; }
    char tmp[16];
    int i = 0;
    if (errnum == 0) { tmp[i++] = '0'; }
    else { while (errnum > 0) { tmp[i++] = '0' + errnum % 10; errnum /= 10; } }
    while (i-- > 0) *p++ = tmp[i];
    *p = '\0';
    return buf;
}
```

Add `#include <stdlib.h>` to the includes at top of `libc/string.c` (needed for `malloc` in `strdup`/`strndup`).

**Step 3: Build and verify**

Run: `make clean && make`
Expected: No errors.

**Step 4: Commit**

```bash
git add libc/include/string.h libc/string.c
git commit -m "ENH: Added 10 missing string functions to libc"
```

---

## Task 5: Add `setenv`/`unsetenv`/`putenv` to stdlib

**Files:**
- Modify: `libc/include/stdlib.h:36` (after `getenv` declaration)
- Modify: `libc/stdlib.c:287` (append after `getenv`)

**Step 1: Add declarations to `stdlib.h`**

Insert after the `getenv` declaration (line 36):

```c
int    setenv(const char *name, const char *value, int overwrite);
int    unsetenv(const char *name);
int    putenv(char *string);
```

**Step 2: Add implementations to `stdlib.c`**

Append at end of file:

```c
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
    env_storage[i] = NULL;
    environ = env_storage;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '='))
        return -1;
    env_ensure_init();
    size_t nlen = strlen(name);
    /* find existing */
    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=') {
            if (!overwrite) return 0;
            /* build new entry */
            static char buf[320];
            strcpy(buf, name);
            strcat(buf, "=");
            strcat(buf, value);
            environ[i] = buf;
            return 0;
        }
    }
    /* add new */
    int count = 0;
    while (environ[count]) count++;
    if (count >= MAX_ENV) return -1;
    static char newbuf[320];
    strcpy(newbuf, name);
    strcat(newbuf, "=");
    strcat(newbuf, value);
    environ[count] = newbuf;
    environ[count + 1] = NULL;
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
    size_t nlen = eq - string;
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
    environ[count + 1] = NULL;
    return 0;
}
```

**Step 3: Build and verify**

Run: `make clean && make`
Expected: No errors.

**Step 4: Commit**

```bash
git add libc/include/stdlib.h libc/stdlib.c
git commit -m "ENH: Added setenv, unsetenv, putenv to libc stdlib"
```

---

## Task 6: Add missing unistd declarations and `isatty` implementation

**Files:**
- Modify: `libc/include/unistd.h:58` (before `#endif`)
- Modify: `libc/unistd.c:141` (append at end)

**Step 1: Add declarations to `unistd.h`**

Insert before the `#endif` on line 60:

```c
/* Terminal / process group */
int     isatty(int fd);
pid_t   setsid(void);
pid_t   getpgrp(void);
int     setpgid(pid_t pid, pid_t pgid);
pid_t   tcgetpgrp(int fd);
int     tcsetpgrp(int fd, pid_t pgrp);
```

**Step 2: Add implementations to `unistd.c`**

Append at end of file:

```c
int isatty(int fd) {
    /* SYS_ioctl = 29, TIOCGWINSZ = 0x5413 */
    char buf[8];
    long ret = syscall(SYS_ioctl, fd, 0x5413, buf);
    return (ret == 0) ? 1 : 0;
}

pid_t setsid(void) {
    /* No kernel support yet — return current PID as stub */
    return getpid();
}

pid_t getpgrp(void) {
    return getpid();
}

int setpgid(pid_t pid, pid_t pgid) {
    (void)pid; (void)pgid;
    return 0;
}

pid_t tcgetpgrp(int fd) {
    (void)fd;
    return getpid();
}

int tcsetpgrp(int fd, pid_t pgrp) {
    (void)fd; (void)pgrp;
    return 0;
}
```

**Step 3: Build and verify**

Run: `make clean && make`
Expected: No errors.

**Step 4: Commit**

```bash
git add libc/include/unistd.h libc/unistd.c
git commit -m "ENH: Added isatty and process group stubs to libc unistd"
```

---

## Task 7: Add `sig_atomic_t` and `sighandler_t` to `signal.h`

**Files:**
- Modify: `libc/include/signal.h:46` (after `sigset_t` typedef)

**Step 1: Add typedefs**

Insert after line 46 (`typedef unsigned long sigset_t;`):

```c
/* sig_atomic_t — integer type that can be accessed atomically */
typedef volatile int sig_atomic_t;

/* sighandler_t — convenience typedef for signal handler functions */
typedef void (*sighandler_t)(int);
```

**Step 2: Build and verify**

Run: `make clean && make`
Expected: No errors.

**Step 3: Commit**

```bash
git add libc/include/signal.h
git commit -m "ENH: Added sig_atomic_t and sighandler_t to signal.h"
```

---

## Task 8: Add missing libc syscall numbers

**Files:**
- Modify: `libc/include/syscall.h` (add any missing numbers)

Check that these are present (add if missing):

```c
#define SYS_lseek       62
#define SYS_umask       166
#define SYS_unlinkat    35
#define SYS_linkat      37
#define SYS_mkdirat     34
#define SYS_renameat2   276
#define SYS_faccessat   48
#define SYS_setsid      157
#define SYS_setpgid     154
```

**Step 1: Verify what's already defined**

Read `libc/include/syscall.h` and check each number. Add only what's missing. The current file already has SYS_ioctl (29), SYS_fcntl (25), SYS_gettimeofday (169).

**Step 2: Add missing definitions**

Insert in the appropriate sections if absent. Do NOT duplicate existing definitions.

**Step 3: Build and verify**

Run: `make clean && make`
Expected: No errors.

**Step 4: Commit**

```bash
git add libc/include/syscall.h
git commit -m "ENH: Added missing syscall numbers to libc syscall.h"
```
