#ifndef _REGEX_H
#define _REGEX_H

#include <sys/types.h>

/* regex compilation flags */
#define REG_EXTENDED  1
#define REG_ICASE     2
#define REG_NOSUB     4
#define REG_NEWLINE   8

/* regexec flags */
#define REG_NOTBOL    1
#define REG_NOTEOL    2

/* Error codes */
#define REG_NOMATCH   1
#define REG_BADPAT    2
#define REG_ECOLLATE  3
#define REG_ECTYPE    4
#define REG_EESCAPE   5
#define REG_ESUBREG   6
#define REG_EBRACK    7
#define REG_EPAREN    8
#define REG_EBRACE    9
#define REG_BADBR     10
#define REG_ERANGE    11
#define REG_ESPACE    12
#define REG_BADRPT    13

typedef struct {
    size_t re_nsub;       /* number of parenthesized subexpressions */
    void  *__priv;        /* internal compiled regex data */
} regex_t;

typedef struct {
    int rm_so;   /* byte offset from start of string to start of match */
    int rm_eo;   /* byte offset from start of string to end of match */
} regmatch_t;

int    regcomp(regex_t *preg, const char *pattern, int cflags);
int    regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[], int eflags);
size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size);
void   regfree(regex_t *preg);

#endif
