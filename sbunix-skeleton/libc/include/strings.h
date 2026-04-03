#ifndef _STRINGS_H
#define _STRINGS_H
#include <sys/types.h>
#include <locale.h>
#include <string.h>  /* strcasecmp, strncasecmp already here */

void bzero(void *s, size_t n);
void bcopy(const void *src, void *dst, size_t n);
int  bcmp(const void *s1, const void *s2, size_t n);
int  ffs(int i);
int  ffsl(long i);
int  ffsll(long long i);
int  strcasecmp(const char *s1, const char *s2);
int  strncasecmp(const char *s1, const char *s2, size_t n);
int  strcasecmp_l(const char *s1, const char *s2, locale_t locale);
int  strncasecmp_l(const char *s1, const char *s2, size_t n, locale_t locale);

#endif
