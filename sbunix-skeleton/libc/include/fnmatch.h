#ifndef _FNMATCH_H
#define _FNMATCH_H

/* Flags for fnmatch() */
#define FNM_PATHNAME  (1 << 0)  /* * and ? do not match / */
#define FNM_PERIOD    (1 << 1)  /* leading . must be matched explicitly */
#define FNM_NOESCAPE  (1 << 2)  /* disable backslash escaping */
#define FNM_NOMATCH   1         /* return value: no match */

int fnmatch(const char *pattern, const char *string, int flags);

#endif /* _FNMATCH_H */
