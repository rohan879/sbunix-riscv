#ifndef _CTYPE_H
#define _CTYPE_H

#include <locale.h>

static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int isalpha(int c) { return isupper(c) || islower(c); }
static inline int isalnum(int c) { return isalpha(c) || isdigit(c); }
static inline int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static inline int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' ||
           c == '\r' || c == '\f' || c == '\v';
}
static inline int isprint(int c) { return c >= 0x20 && c <= 0x7e; }
static inline int isgraph(int c) { return c > 0x20 && c <= 0x7e; }
static inline int iscntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7f; }
static inline int ispunct(int c) { return isgraph(c) && !isalnum(c); }
static inline int isblank(int c) { return c == ' ' || c == '\t'; }
static inline int isascii(int c) { return (unsigned)c <= 0x7f; }
static inline int toupper(int c) { return islower(c) ? c - 32 : c; }
static inline int tolower(int c) { return isupper(c) ? c + 32 : c; }

static inline int isalnum_l(int c, locale_t l) { (void)l; return isalnum(c); }
static inline int isalpha_l(int c, locale_t l) { (void)l; return isalpha(c); }
static inline int isblank_l(int c, locale_t l) { (void)l; return isblank(c); }
static inline int iscntrl_l(int c, locale_t l) { (void)l; return iscntrl(c); }
static inline int isdigit_l(int c, locale_t l) { (void)l; return isdigit(c); }
static inline int isgraph_l(int c, locale_t l) { (void)l; return isgraph(c); }
static inline int islower_l(int c, locale_t l) { (void)l; return islower(c); }
static inline int isprint_l(int c, locale_t l) { (void)l; return isprint(c); }
static inline int ispunct_l(int c, locale_t l) { (void)l; return ispunct(c); }
static inline int isspace_l(int c, locale_t l) { (void)l; return isspace(c); }
static inline int isupper_l(int c, locale_t l) { (void)l; return isupper(c); }
static inline int isxdigit_l(int c, locale_t l) { (void)l; return isxdigit(c); }
static inline int tolower_l(int c, locale_t l) { (void)l; return tolower(c); }
static inline int toupper_l(int c, locale_t l) { (void)l; return toupper(c); }

#endif /* _CTYPE_H */
