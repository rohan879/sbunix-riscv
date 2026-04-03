#ifndef _WORDEXP_H
#define _WORDEXP_H

#include <sys/types.h>

typedef struct {
  size_t we_wordc;
  char **we_wordv;
  size_t we_offs;
} wordexp_t;

#define WRDE_APPEND  0x01
#define WRDE_DOOFFS  0x02
#define WRDE_NOCMD   0x04
#define WRDE_REUSE   0x08
#define WRDE_SHOWERR 0x10
#define WRDE_UNDEF   0x20

#define WRDE_NOSYS   1
#define WRDE_NOSPACE 2
#define WRDE_BADCHAR 3
#define WRDE_BADVAL  4
#define WRDE_CMDSUB  5
#define WRDE_SYNTAX  6

int wordexp(const char *words, wordexp_t *pwordexp, int flags);
void wordfree(wordexp_t *pwordexp);

#endif
