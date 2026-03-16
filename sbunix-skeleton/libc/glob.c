/*
 * glob.c — POSIX glob() and globfree() for SBUnix libc.
 *
 * Expands file-name patterns by reading directories and matching
 * entries with fnmatch().
 */

#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fnmatch.h>
#include <errno.h>

/* Simple strcmp-based comparator for qsort */
static int glob_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int glob(const char *pattern, int flags,
         int (*errfunc)(const char *, int),
         glob_t *pglob)
{
    if (!pattern || !pglob)
        return GLOB_NOMATCH;

    /* ---------------------------------------------------------------
     * Split pattern into directory part and filename (base) part
     * at the last '/'.  If there is no '/', the directory is "."
     * and the base is the whole pattern.
     * --------------------------------------------------------------- */
    char dir[256];
    const char *base;

    const char *slash = (void *)0;
    for (const char *p = pattern; *p; p++) {
        if (*p == '/')
            slash = p;
    }

    if (slash) {
        size_t dlen = (size_t)(slash - pattern);
        if (dlen == 0) {
            /* Pattern starts with '/' (e.g. "/foo.c") */
            dir[0] = '/';
            dir[1] = '\0';
        } else {
            if (dlen >= sizeof(dir))
                dlen = sizeof(dir) - 1;
            memcpy(dir, pattern, dlen);
            dir[dlen] = '\0';
        }
        base = slash + 1;
    } else {
        dir[0] = '.';
        dir[1] = '\0';
        base = pattern;
    }

    /* ---------------------------------------------------------------
     * Initialise or prepare for GLOB_APPEND
     * --------------------------------------------------------------- */
    size_t old_pathc = 0;
    char **old_pathv = (void *)0;

    if (flags & GLOB_APPEND) {
        old_pathc = pglob->gl_pathc;
        old_pathv = pglob->gl_pathv;
    } else {
        pglob->gl_pathc = 0;
        pglob->gl_pathv = (void *)0;
    }

    /* ---------------------------------------------------------------
     * Read directory and collect matches
     * --------------------------------------------------------------- */
    DIR *dp = opendir(dir);
    if (!dp) {
        if (errfunc) {
            if (errfunc(dir, errno))
                return GLOB_ABORTED;
        }
        if (flags & GLOB_ERR)
            return GLOB_ABORTED;
        /* No directory — treat as no matches */
        if (flags & GLOB_NOCHECK)
            goto nocheck;
        return GLOB_NOMATCH;
    }

    /* Dynamic array of matched paths */
    size_t cap = 16;
    size_t count = 0;
    char **results = malloc(cap * sizeof(char *));
    if (!results) {
        closedir(dp);
        return GLOB_NOSPACE;
    }

    int fnm_flags = 0;
    if (flags & GLOB_NOESCAPE)
        fnm_flags |= FNM_NOESCAPE;

    struct dirent *de;
    while ((de = readdir(dp)) != (void *)0) {
        /* Skip "." and ".." */
        if (de->d_name[0] == '.' && (de->d_name[1] == '\0' ||
            (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;

        /* Skip hidden files unless the pattern explicitly starts with '.' */
        if (de->d_name[0] == '.' && base[0] != '.')
            continue;

        if (fnmatch(base, de->d_name, fnm_flags) == 0) {
            /* Build full path: dir/name */
            size_t dlen = strlen(dir);
            size_t nlen = strlen(de->d_name);
            int need_slash = (dir[dlen - 1] != '/');
            size_t plen = dlen + need_slash + nlen + 1;

            char *path = malloc(plen);
            if (!path) {
                /* Free what we've collected so far */
                for (size_t i = 0; i < count; i++)
                    free(results[i]);
                free(results);
                closedir(dp);
                return GLOB_NOSPACE;
            }

            memcpy(path, dir, dlen);
            if (need_slash)
                path[dlen] = '/';
            memcpy(path + dlen + need_slash, de->d_name, nlen + 1);

            /* Grow the results array if needed */
            if (count >= cap) {
                size_t newcap = cap * 2;
                char **tmp = realloc(results, newcap * sizeof(char *));
                if (!tmp) {
                    free(path);
                    for (size_t i = 0; i < count; i++)
                        free(results[i]);
                    free(results);
                    closedir(dp);
                    return GLOB_NOSPACE;
                }
                results = tmp;
                cap = newcap;
            }

            results[count++] = path;
        }
    }

    closedir(dp);

    /* ---------------------------------------------------------------
     * No matches
     * --------------------------------------------------------------- */
    if (count == 0) {
        free(results);
        if (flags & GLOB_NOCHECK)
            goto nocheck;
        return GLOB_NOMATCH;
    }

    /* ---------------------------------------------------------------
     * Sort results unless GLOB_NOSORT
     * --------------------------------------------------------------- */
    if (!(flags & GLOB_NOSORT))
        qsort(results, count, sizeof(char *), glob_cmp);

    /* ---------------------------------------------------------------
     * Build final gl_pathv (with NULL sentinel)
     * --------------------------------------------------------------- */
    size_t total = old_pathc + count;
    char **pathv = malloc((total + 1) * sizeof(char *));
    if (!pathv) {
        for (size_t i = 0; i < count; i++)
            free(results[i]);
        free(results);
        return GLOB_NOSPACE;
    }

    /* Copy old entries (for GLOB_APPEND) */
    for (size_t i = 0; i < old_pathc; i++)
        pathv[i] = old_pathv[i];

    /* Copy new entries */
    for (size_t i = 0; i < count; i++)
        pathv[old_pathc + i] = results[i];

    pathv[total] = (void *)0;

    /* Free old pathv array (but not the strings — they're reused) */
    if (old_pathv)
        free(old_pathv);
    free(results);

    pglob->gl_pathc = total;
    pglob->gl_pathv = pathv;
    return 0;

nocheck:
    /* GLOB_NOCHECK: return the pattern itself as the single match */
    {
        size_t total = old_pathc + 1;
        char **pathv = malloc((total + 1) * sizeof(char *));
        if (!pathv)
            return GLOB_NOSPACE;

        for (size_t i = 0; i < old_pathc; i++)
            pathv[i] = old_pathv[i];

        size_t plen = strlen(pattern);
        char *copy = malloc(plen + 1);
        if (!copy) {
            free(pathv);
            return GLOB_NOSPACE;
        }
        memcpy(copy, pattern, plen + 1);

        pathv[old_pathc] = copy;
        pathv[total] = (void *)0;

        if (old_pathv)
            free(old_pathv);

        pglob->gl_pathc = total;
        pglob->gl_pathv = pathv;
    }
    return 0;
}

void globfree(glob_t *pglob) {
    if (!pglob || !pglob->gl_pathv)
        return;
    for (size_t i = 0; i < pglob->gl_pathc; i++)
        free(pglob->gl_pathv[i]);
    free(pglob->gl_pathv);
    pglob->gl_pathv = (void *)0;
    pglob->gl_pathc = 0;
}
