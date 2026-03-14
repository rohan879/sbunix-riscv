/*
 * glob.c — pathname pattern expansion for SBUnix libc.
 *
 * Expands shell-style glob patterns against the filesystem using
 * opendir/readdir/fnmatch.  Supports *, ?, and [...] in the last
 * path component.
 *
 * Flags: GLOB_APPEND, GLOB_NOSORT, GLOB_NOCHECK
 */

#include <glob.h>
#include <fnmatch.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Check if a string contains glob metacharacters */
static int has_magic(const char *s) {
    for (; *s; s++) {
        if (*s == '*' || *s == '?' || *s == '[')
            return 1;
    }
    return 0;
}

/* Add a path to pglob->gl_pathv, reallocating as needed */
static int glob_add(glob_t *pglob, const char *path) {
    size_t count = pglob->gl_pathc + 1;
    char **nv = (char **)realloc(pglob->gl_pathv,
                                  (count + 1) * sizeof(char *));
    if (!nv)
        return GLOB_NOSPACE;
    pglob->gl_pathv = nv;

    char *dup = strdup(path);
    if (!dup)
        return GLOB_NOSPACE;

    pglob->gl_pathv[pglob->gl_pathc] = dup;
    pglob->gl_pathc = count;
    pglob->gl_pathv[count] = (void *)0;  /* NULL-terminate the list */
    return 0;
}

/* strcmp wrapper for qsort */
static int cmp_strings(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

int glob(const char *pattern, int flags,
         int (*errfunc)(const char *, int),
         glob_t *pglob) {

    /* Initialize if not appending */
    if (!(flags & GLOB_APPEND)) {
        pglob->gl_pathc = 0;
        pglob->gl_pathv = (void *)0;
        pglob->gl_offs  = 0;
    }

    size_t old_pathc = pglob->gl_pathc;

    /* No metacharacters — just check if file exists or use NOCHECK */
    if (!has_magic(pattern)) {
        int ret = glob_add(pglob, pattern);
        if (ret != 0) return ret;
        return 0;
    }

    /* Split pattern into directory and filename components */
    const char *slash = (void *)0;
    for (const char *p = pattern; *p; p++) {
        if (*p == '/')
            slash = p;
    }

    char dir[1024];
    const char *base_pattern;

    if (slash) {
        size_t dlen = (size_t)(slash - pattern);
        if (dlen == 0) {
            dir[0] = '/';
            dir[1] = '\0';
        } else {
            if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
            memcpy(dir, pattern, dlen);
            dir[dlen] = '\0';
        }
        base_pattern = slash + 1;
    } else {
        dir[0] = '.';
        dir[1] = '\0';
        base_pattern = pattern;
    }

    /* If the directory part itself has globs, we don't handle recursive
     * expansion — just try opening it literally.  This covers the common
     * case where only the filename component has globs. */

    DIR *dp = opendir(dir);
    if (!dp) {
        if (errfunc && errfunc(dir, errno))
            return GLOB_ABORTED;
        if (flags & GLOB_NOCHECK)
            return glob_add(pglob, pattern);
        return GLOB_NOMATCH;
    }

    struct dirent *ent;
    while ((ent = readdir(dp)) != (void *)0) {
        /* Skip . and .. */
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        /* Skip hidden files unless pattern starts with . */
        if (ent->d_name[0] == '.' && base_pattern[0] != '.')
            continue;

        if (fnmatch(base_pattern, ent->d_name, 0) == 0) {
            /* Build full path */
            char fullpath[1024];
            if (slash) {
                size_t dlen = strlen(dir);
                if (dlen + 1 + strlen(ent->d_name) < sizeof(fullpath)) {
                    memcpy(fullpath, dir, dlen);
                    fullpath[dlen] = '/';
                    strcpy(fullpath + dlen + 1, ent->d_name);
                } else {
                    continue;  /* path too long, skip */
                }
            } else {
                if (strlen(ent->d_name) < sizeof(fullpath))
                    strcpy(fullpath, ent->d_name);
                else
                    continue;
            }
            int ret = glob_add(pglob, fullpath);
            if (ret != 0) {
                closedir(dp);
                return ret;
            }
        }
    }

    closedir(dp);

    /* No new matches found? */
    if (pglob->gl_pathc == old_pathc) {
        if (flags & GLOB_NOCHECK)
            return glob_add(pglob, pattern);
        return GLOB_NOMATCH;
    }

    /* Sort results (unless GLOB_NOSORT) */
    if (!(flags & GLOB_NOSORT) && pglob->gl_pathc > old_pathc) {
        qsort(pglob->gl_pathv + old_pathc,
              pglob->gl_pathc - old_pathc,
              sizeof(char *), cmp_strings);
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
