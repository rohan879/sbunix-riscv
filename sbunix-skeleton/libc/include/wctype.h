#ifndef _WCTYPE_H
#define _WCTYPE_H

#include <wchar.h>
#include <locale.h>

typedef unsigned long wctype_t;
typedef unsigned long wctrans_t;

int iswalnum(wint_t wc);
int iswalpha(wint_t wc);
int iswblank(wint_t wc);
int iswcntrl(wint_t wc);
int iswdigit(wint_t wc);
int iswgraph(wint_t wc);
int iswlower(wint_t wc);
int iswprint(wint_t wc);
int iswpunct(wint_t wc);
int iswspace(wint_t wc);
int iswupper(wint_t wc);
int iswxdigit(wint_t wc);

int iswalnum_l(wint_t wc, locale_t l);
int iswalpha_l(wint_t wc, locale_t l);
int iswblank_l(wint_t wc, locale_t l);
int iswcntrl_l(wint_t wc, locale_t l);
int iswdigit_l(wint_t wc, locale_t l);
int iswgraph_l(wint_t wc, locale_t l);
int iswlower_l(wint_t wc, locale_t l);
int iswprint_l(wint_t wc, locale_t l);
int iswpunct_l(wint_t wc, locale_t l);
int iswspace_l(wint_t wc, locale_t l);
int iswupper_l(wint_t wc, locale_t l);
int iswxdigit_l(wint_t wc, locale_t l);

int iswctype(wint_t wc, wctype_t desc);
int iswctype_l(wint_t wc, wctype_t desc, locale_t l);

wint_t towlower(wint_t wc);
wint_t towupper(wint_t wc);
wint_t towlower_l(wint_t wc, locale_t l);
wint_t towupper_l(wint_t wc, locale_t l);
wint_t towctrans(wint_t wc, wctrans_t desc);
wint_t towctrans_l(wint_t wc, wctrans_t desc, locale_t l);

wctype_t wctype(const char *name);
wctype_t wctype_l(const char *name, locale_t l);
wctrans_t wctrans(const char *name);
wctrans_t wctrans_l(const char *name, locale_t l);

#endif
