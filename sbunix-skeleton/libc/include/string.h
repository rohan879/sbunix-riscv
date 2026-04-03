#ifndef _STRING_H
#define _STRING_H

#include <sys/types.h>
#include <stddef.h>
#include <locale.h>

size_t strlen(const char *s);
int    strcmp(const char *s1, const char *s2);
int    strncmp(const char *s1, const char *s2, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
void  *memset(void *s, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
int    memcmp(const void *s1, const void *s2, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memchr(const void *s, int c, size_t n);
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);
char  *strncat(char *dst, const char *src, size_t n);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strpbrk(const char *s, const char *accept);
char  *strtok(char *str, const char *delim);
char  *strerror(int errnum);
int    strcasecmp(const char *s1, const char *s2);
int    strncasecmp(const char *s1, const char *s2, size_t n);
size_t strnlen(const char *s, size_t maxlen);
char  *stpcpy(char *dst, const char *src);
char  *stpncpy(char *dst, const char *src, size_t n);
char  *strsignal(int sig);
char  *strtok_r(char *str, const char *delim, char **saveptr);
int    strerror_r(int errnum, char *buf, size_t buflen);

void  *memccpy(void *dst, const void *src, int c, size_t n);
void  *memmem(const void *haystack, size_t haystacklen,
              const void *needle, size_t needlelen);
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
int    strcoll(const char *s1, const char *s2);
int    strcoll_l(const char *s1, const char *s2, locale_t locale);
size_t strxfrm(char *dst, const char *src, size_t n);
size_t strxfrm_l(char *dst, const char *src, size_t n, locale_t locale);
char  *strerror_l(int errnum, locale_t locale);

#endif
