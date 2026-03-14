#ifndef _GLOB_H
#define _GLOB_H

#include <sys/types.h>

/* Flags for glob() */
#define GLOB_APPEND   (1 << 0)  /* append to existing results */
#define GLOB_NOSORT   (1 << 1)  /* do not sort results */
#define GLOB_NOCHECK  (1 << 2)  /* if no match, return pattern itself */
#define GLOB_NOESCAPE (1 << 3)  /* disable backslash escaping */

/* Return values */
#define GLOB_NOSPACE  1  /* memory allocation failure */
#define GLOB_ABORTED  2  /* read error */
#define GLOB_NOMATCH  3  /* no matches found */

typedef struct {
    size_t  gl_pathc;   /* count of matched paths */
    char  **gl_pathv;   /* list of matched pathnames */
    size_t  gl_offs;    /* reserved slots at beginning of gl_pathv */
} glob_t;

int  glob(const char *pattern, int flags,
          int (*errfunc)(const char *, int), glob_t *pglob);
void globfree(glob_t *pglob);

#endif /* _GLOB_H */
