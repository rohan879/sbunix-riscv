#include <search.h>

#include <stdlib.h>
#include <string.h>

typedef struct hnode {
  ENTRY item;
  struct hnode *next;
} hnode_t;

static hnode_t *hlist;

int hcreate(size_t nel) {
  (void)nel;
  hlist = (void *)0;
  return 1;
}

void hdestroy(void) {
  hnode_t *n = hlist;
  while (n) {
    hnode_t *next = n->next;
    free(n);
    n = next;
  }
  hlist = (void *)0;
}

ENTRY *hsearch(ENTRY item, ACTION action) {
  for (hnode_t *n = hlist; n; n = n->next) {
    if (strcmp(n->item.key, item.key) == 0)
      return &n->item;
  }
  if (action == FIND)
    return (void *)0;

  hnode_t *n = (hnode_t *)malloc(sizeof(*n));
  if (!n)
    return (void *)0;
  n->item = item;
  n->next = hlist;
  hlist = n;
  return &n->item;
}

void *lfind(const void *key, const void *base, size_t *nmemb, size_t size,
            int (*compar)(const void *, const void *)) {
  const char *p = (const char *)base;
  for (size_t i = 0; i < *nmemb; i++) {
    const void *elem = p + i * size;
    if (compar(key, elem) == 0)
      return (void *)elem;
  }
  return (void *)0;
}

void *lsearch(const void *key, void *base, size_t *nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
  void *found = lfind(key, base, nmemb, size, compar);
  if (found)
    return found;
  char *p = (char *)base + (*nmemb) * size;
  memcpy(p, key, size);
  (*nmemb)++;
  return p;
}

typedef struct qnode {
  struct qnode *next;
  struct qnode *prev;
} qnode_t;

void insque(void *element, void *pred) {
  qnode_t *e = (qnode_t *)element;
  qnode_t *p = (qnode_t *)pred;
  if (!p) {
    e->next = (void *)0;
    e->prev = (void *)0;
    return;
  }
  e->next = p->next;
  e->prev = p;
  if (p->next)
    p->next->prev = e;
  p->next = e;
}

void remque(void *element) {
  qnode_t *e = (qnode_t *)element;
  if (e->prev)
    e->prev->next = e->next;
  if (e->next)
    e->next->prev = e->prev;
  e->next = (void *)0;
  e->prev = (void *)0;
}

typedef struct tnode {
  const void *key;
  struct tnode *left;
  struct tnode *right;
} tnode_t;

typedef void posix_tnode;

posix_tnode *tsearch(const void *key, posix_tnode **rootp,
                     int (*compar)(const void *, const void *)) {
  tnode_t **cur = (tnode_t **)rootp;
  while (*cur) {
    int c = compar(key, (*cur)->key);
    if (c == 0)
      return *cur;
    cur = (c < 0) ? &(*cur)->left : &(*cur)->right;
  }
  tnode_t *n = (tnode_t *)malloc(sizeof(*n));
  if (!n)
    return (void *)0;
  n->key = key;
  n->left = (void *)0;
  n->right = (void *)0;
  *cur = n;
  return n;
}

posix_tnode *tfind(const void *key, posix_tnode *const *rootp,
                   int (*compar)(const void *, const void *)) {
  tnode_t *cur = *(tnode_t *const *)rootp;
  while (cur) {
    int c = compar(key, cur->key);
    if (c == 0)
      return cur;
    cur = (c < 0) ? cur->left : cur->right;
  }
  return (void *)0;
}

static tnode_t *tdelete_impl(const void *key, tnode_t *root,
                             int (*compar)(const void *, const void *),
                             void **deleted) {
  if (!root)
    return (void *)0;
  int c = compar(key, root->key);
  if (c < 0) {
    root->left = tdelete_impl(key, root->left, compar, deleted);
    return root;
  }
  if (c > 0) {
    root->right = tdelete_impl(key, root->right, compar, deleted);
    return root;
  }

  *deleted = root;
  if (!root->left) {
    tnode_t *r = root->right;
    free(root);
    return r;
  }
  if (!root->right) {
    tnode_t *l = root->left;
    free(root);
    return l;
  }
  tnode_t *p = root;
  tnode_t *s = root->right;
  while (s->left) {
    p = s;
    s = s->left;
  }
  root->key = s->key;
  if (p == root)
    p->right = s->right;
  else
    p->left = s->right;
  free(s);
  return root;
}

void *tdelete(const void *key, posix_tnode **rootp,
              int (*compar)(const void *, const void *)) {
  void *deleted = (void *)0;
  *rootp = tdelete_impl(key, (tnode_t *)*rootp, compar, &deleted);
  return deleted;
}

static void twalk_impl(const tnode_t *node,
                       void (*action)(const void *, VISIT, int),
                       int depth) {
  if (!node)
    return;
  if (!node->left && !node->right) {
    action(node, leaf, depth);
    return;
  }
  action(node, preorder, depth);
  if (node->left)
    twalk_impl(node->left, action, depth + 1);
  action(node, postorder, depth);
  if (node->right)
    twalk_impl(node->right, action, depth + 1);
  action(node, endorder, depth);
}

void twalk(const posix_tnode *root,
           void (*action)(const posix_tnode *nodep, VISIT which, int depth)) {
  if (!root || !action)
    return;
  twalk_impl((const tnode_t *)root, (void (*)(const void *, VISIT, int))action, 0);
}
