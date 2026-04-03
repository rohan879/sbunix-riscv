#include <wordexp.h>
#include <stdlib.h>
#include <string.h>

static char *dup_cstr(const char *s) {
  size_t n = strlen(s);
  char *d = (char *)malloc(n + 1);
  if (!d)
    return (void *)0;
  memcpy(d, s, n + 1);
  return d;
}

int wordexp(const char *words, wordexp_t *pwordexp, int flags) {
  (void)flags;
  if (!words || !pwordexp)
    return WRDE_BADVAL;

  if (!(flags & WRDE_REUSE)) {
    pwordexp->we_wordc = 0;
    pwordexp->we_wordv = (void *)0;
    pwordexp->we_offs = 0;
  }

  size_t total = 1 + pwordexp->we_offs + 1;
  char **vec = (char **)calloc(total, sizeof(char *));
  if (!vec)
    return WRDE_NOSPACE;

  size_t idx = pwordexp->we_offs;
  vec[idx] = dup_cstr(words);
  if (!vec[idx]) {
    free(vec);
    return WRDE_NOSPACE;
  }

  pwordexp->we_wordc = 1;
  pwordexp->we_wordv = vec;
  return 0;
}

void wordfree(wordexp_t *pwordexp) {
  if (!pwordexp || !pwordexp->we_wordv)
    return;
  for (size_t i = 0; pwordexp->we_wordv[i]; i++)
    free(pwordexp->we_wordv[i]);
  free(pwordexp->we_wordv);
  pwordexp->we_wordv = (void *)0;
  pwordexp->we_wordc = 0;
}
