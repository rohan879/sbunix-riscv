/*
 * fnmatch.c — POSIX fnmatch() for SBUnix libc.
 */

#include <string.h>

#define FNM_NOMATCH 1
#define FNM_PATHNAME 0x01
#define FNM_NOESCAPE 0x02
#define FNM_PERIOD 0x04

int fnmatch(const char *pattern, const char *string, int flags) {
    const char *p = pattern, *s = string;

    while (*p) {
        switch (*p) {
        case '?':
            if (*s == '\0') return FNM_NOMATCH;
            if ((flags & FNM_PATHNAME) && *s == '/') return FNM_NOMATCH;
            if ((flags & FNM_PERIOD) && *s == '.' &&
                (s == string || ((flags & FNM_PATHNAME) && s[-1] == '/')))
                return FNM_NOMATCH;
            s++;
            p++;
            break;

        case '*': {
            /* Skip consecutive stars */
            while (*p == '*') p++;
            if (*p == '\0') {
                if (flags & FNM_PATHNAME) {
                    return strchr(s, '/') ? FNM_NOMATCH : 0;
                }
                return 0;
            }
            while (*s) {
                if (fnmatch(p, s, flags & ~FNM_PERIOD) == 0)
                    return 0;
                if ((flags & FNM_PATHNAME) && *s == '/')
                    break;
                s++;
            }
            return FNM_NOMATCH;
        }

        case '[': {
            if (*s == '\0') return FNM_NOMATCH;
            p++;
            int negate = 0;
            if (*p == '!' || *p == '^') { negate = 1; p++; }
            int matched = 0;
            char c = *s;
            while (*p && *p != ']') {
                char lo = *p++;
                if (*p == '-' && p[1] != ']' && p[1] != '\0') {
                    p++;
                    char hi = *p++;
                    if (c >= lo && c <= hi) matched = 1;
                } else {
                    if (c == lo) matched = 1;
                }
            }
            if (*p == ']') p++;
            if (negate) matched = !matched;
            if (!matched) return FNM_NOMATCH;
            s++;
            break;
        }

        case '\\':
            if (!(flags & FNM_NOESCAPE) && p[1]) p++;
            /* FALLTHROUGH */
        default:
            if (*p != *s) return FNM_NOMATCH;
            p++;
            s++;
            break;
        }
    }

    return (*s == '\0') ? 0 : FNM_NOMATCH;
}
