#include <string.h>
#include <stdlib.h>

size_t strlen(const char *s) {
    size_t i = 0;
    while (s[i])
        i++;
    return i;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0)
        return 0;
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (n && (*d++ = *src++))
        n--;
    while (n--)
        *d++ = '\0';
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst;
    while (*d)
        d++;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    if (c == '\0')
        return (char *)s;
    return NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c)
            last = s;
        s++;
    }
    if (c == '\0')
        return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle)
        return (char *)haystack;
    while (*haystack) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n)
            return (char *)haystack;
        haystack++;
    }
    return NULL;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *a = s1;
    const unsigned char *b = s2;
    while (n--) {
        if (*a != *b)
            return *a - *b;
        a++;
        b++;
    }
    return 0;
}

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

static int tolower_char(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && tolower_char((unsigned char)*s1) == tolower_char((unsigned char)*s2)) {
        s1++;
        s2++;
    }
    return tolower_char((unsigned char)*s1) - tolower_char((unsigned char)*s2);
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && tolower_char((unsigned char)*s1) == tolower_char((unsigned char)*s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return tolower_char((unsigned char)*s1) - tolower_char((unsigned char)*s2);
}

char *strerror(int errnum) {
    switch (errnum) {
    case 0:  return "Success";
    case 1:  return "Operation not permitted";
    case 2:  return "No such file or directory";
    case 3:  return "No such process";
    case 4:  return "Interrupted system call";
    case 5:  return "Input/output error";
    case 6:  return "No such device or address";
    case 7:  return "Argument list too long";
    case 8:  return "Exec format error";
    case 9:  return "Bad file descriptor";
    case 10: return "No child processes";
    case 11: return "Resource temporarily unavailable";
    case 12: return "Cannot allocate memory";
    case 13: return "Permission denied";
    case 14: return "Bad address";
    case 16: return "Device or resource busy";
    case 17: return "File exists";
    case 19: return "No such device";
    case 20: return "Not a directory";
    case 21: return "Is a directory";
    case 22: return "Invalid argument";
    case 23: return "Too many open files in system";
    case 24: return "Too many open files";
    case 25: return "Inappropriate ioctl for device";
    case 27: return "File too large";
    case 28: return "No space left on device";
    case 29: return "Illegal seek";
    case 30: return "Read-only file system";
    case 31: return "Too many links";
    case 32: return "Broken pipe";
    case 36: return "File name too long";
    case 38: return "Function not implemented";
    case 39: return "Directory not empty";
    default: {
        static char buf[32];
        char *p = buf;
        char *prefix = "Unknown error ";
        while (*prefix)
            *p++ = *prefix++;
        if (errnum < 0) { *p++ = '-'; errnum = -errnum; }
        char tmp[16];
        int i = 0;
        if (errnum == 0) {
            tmp[i++] = '0';
        } else {
            while (errnum > 0) {
                tmp[i++] = '0' + errnum % 10;
                errnum /= 10;
            }
        }
        while (i-- > 0)
            *p++ = tmp[i];
        *p = '\0';
        return buf;
    }
    }
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t i = 0;
    while (i < maxlen && s[i])
        i++;
    return i;
}

char *stpcpy(char *dst, const char *src) {
    while ((*dst = *src) != '\0') {
        dst++;
        src++;
    }
    return dst;
}

char *stpncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++)
        dst[i] = src[i];
    char *end = dst + i;
    for (; i < n; i++)
        dst[i] = '\0';
    return end;
}

static const char *sig_names[] = {
    [0]  = "Signal 0",
    [1]  = "Hangup",
    [2]  = "Interrupt",
    [3]  = "Quit",
    [4]  = "Illegal instruction",
    [5]  = "Trace/breakpoint trap",
    [6]  = "Aborted",
    [7]  = "Bus error",
    [8]  = "Floating point exception",
    [9]  = "Killed",
    [10] = "User defined signal 1",
    [11] = "Segmentation fault",
    [12] = "User defined signal 2",
    [13] = "Broken pipe",
    [14] = "Alarm clock",
    [15] = "Terminated",
    [17] = "Child exited",
    [18] = "Continued",
    [19] = "Stopped (signal)",
    [20] = "Stopped",
};

char *strsignal(int sig) {
    static char buf[32];
    if (sig >= 0 && sig <= 20 && sig_names[sig])
        return (char *)sig_names[sig];
    char *p = buf;
    char *prefix = "Signal ";
    while (*prefix) *p++ = *prefix++;
    if (sig < 0) { *p++ = '-'; sig = -sig; }
    char tmp[16];
    int i = 0;
    if (sig == 0) { tmp[i++] = '0'; }
    else { while (sig > 0) { tmp[i++] = '0' + sig % 10; sig /= 10; } }
    while (i-- > 0) *p++ = tmp[i];
    *p = '\0';
    return buf;
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
    if (!str) str = *saveptr;
    if (!str) return NULL;
    /* Skip leading delimiters */
    while (*str && strchr(delim, *str)) str++;
    if (!*str) { *saveptr = NULL; return NULL; }
    char *start = str;
    while (*str && !strchr(delim, *str)) str++;
    if (*str) { *str = '\0'; *saveptr = str + 1; }
    else { *saveptr = NULL; }
    return start;
}

int strerror_r(int errnum, char *buf, size_t buflen) {
    char *msg = strerror(errnum);
    size_t len = strlen(msg);
    if (len >= buflen) { if (buflen > 0) { memcpy(buf, msg, buflen - 1); buf[buflen - 1] = '\0'; } return -1; }
    memcpy(buf, msg, len + 1);
    return 0;
}
