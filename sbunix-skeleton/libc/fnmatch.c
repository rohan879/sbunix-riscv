/*
 * fnmatch.c — shell-style pattern matching for SBUnix libc.
 *
 * Supports:
 *   *       — match any sequence of characters
 *   ?       — match any single character
 *   [...]   — match any character in the set
 *   [^...]  — match any character NOT in the set
 *
 * Flags:
 *   FNM_PATHNAME  — * and ? do not match /
 *   FNM_PERIOD    — leading . must be matched explicitly
 */

#include <fnmatch.h>
#include <stddef.h>

static int _match(const char *pat, const char *str, int flags, int at_start) {
    while (*pat) {
        if (!*str) {
            /* String exhausted: only match if rest of pattern is all '*' */
            while (*pat == '*') pat++;
            return *pat ? FNM_NOMATCH : 0;
        }

        /* FNM_PERIOD: leading dot must be matched explicitly */
        if ((flags & FNM_PERIOD) && *str == '.' && at_start) {
            if (*pat != '.')
                return FNM_NOMATCH;
            pat++;
            str++;
            at_start = 0;
            continue;
        }

        switch (*pat) {
        case '?':
            /* Match any single character, but not / if FNM_PATHNAME */
            if ((flags & FNM_PATHNAME) && *str == '/')
                return FNM_NOMATCH;
            pat++;
            str++;
            at_start = 0;
            break;

        case '*':
            /* Skip consecutive '*' */
            while (*pat == '*') pat++;

            /* Trailing '*' matches everything (except / with FNM_PATHNAME) */
            if (!*pat) {
                if (flags & FNM_PATHNAME) {
                    /* Must not contain / in rest of string */
                    while (*str) {
                        if (*str == '/') return FNM_NOMATCH;
                        str++;
                    }
                }
                return 0;
            }

            /* Try matching the rest of the pattern at each position */
            for (; *str; str++) {
                if ((flags & FNM_PATHNAME) && *str == '/')
                    break;
                if (_match(pat, str, flags & ~FNM_PERIOD, 0) == 0)
                    return 0;
            }
            /* Also try matching at the current exhausted position */
            return _match(pat, str, flags & ~FNM_PERIOD, 0);

        case '[': {
            /* Character class */
            if ((flags & FNM_PATHNAME) && *str == '/')
                return FNM_NOMATCH;

            pat++;
            int negate = 0;
            if (*pat == '!' || *pat == '^') {
                negate = 1;
                pat++;
            }

            int matched = 0;
            int c = (unsigned char)*str;

            /* Empty class [] is invalid — treat as literal */
            if (*pat == ']') {
                if (c == ']') matched = 1;
                pat++;
            }

            while (*pat && *pat != ']') {
                int lo = (unsigned char)*pat;
                pat++;
                if (*pat == '-' && *(pat + 1) && *(pat + 1) != ']') {
                    pat++; /* skip '-' */
                    int hi = (unsigned char)*pat;
                    pat++;
                    if (c >= lo && c <= hi) matched = 1;
                } else {
                    if (c == lo) matched = 1;
                }
            }
            if (*pat == ']') pat++;

            if (negate) matched = !matched;
            if (!matched) return FNM_NOMATCH;

            str++;
            at_start = 0;
            break;
        }

        default:
            if (*pat != *str)
                return FNM_NOMATCH;
            pat++;
            str++;
            at_start = 0;
            break;
        }
    }

    return *str ? FNM_NOMATCH : 0;
}

int fnmatch(const char *pattern, const char *string, int flags) {
    return _match(pattern, string, flags, 1);
}
