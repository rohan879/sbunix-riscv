#ifndef _SEARCH_H
#define _SEARCH_H

#include <sys/types.h>

typedef enum {
  FIND,
  ENTER
} ACTION;

typedef enum {
  preorder,
  postorder,
  endorder,
  leaf
} VISIT;

typedef void posix_tnode;

typedef struct entry {
  char *key;
  void *data;
} ENTRY;

int hcreate(size_t nel);
void hdestroy(void);
ENTRY *hsearch(ENTRY item, ACTION action);

void *lfind(const void *key, const void *base, size_t *nmemb, size_t size,
            int (*compar)(const void *, const void *));
void *lsearch(const void *key, void *base, size_t *nmemb, size_t size,
              int (*compar)(const void *, const void *));

void insque(void *element, void *pred);
void remque(void *element);

posix_tnode *tsearch(const void *key, posix_tnode **rootp,
                     int (*compar)(const void *, const void *));
posix_tnode *tfind(const void *key, posix_tnode *const *rootp,
                   int (*compar)(const void *, const void *));
void *tdelete(const void *key, posix_tnode **rootp,
              int (*compar)(const void *, const void *));
void twalk(const posix_tnode *root,
           void (*action)(const posix_tnode *nodep, VISIT which, int depth));

#endif
