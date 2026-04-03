#ifndef _UCHAR_H
#define _UCHAR_H

#include <stddef.h>
#include <wchar.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
#define UCHAR_RESTRICT
#else
#define UCHAR_RESTRICT restrict
#endif

#ifndef __char16_t_defined
#ifndef __cplusplus
typedef uint16_t char16_t;
#endif
#define __char16_t_defined
#endif

#ifndef __char32_t_defined
#ifndef __cplusplus
typedef uint32_t char32_t;
#endif
#define __char32_t_defined
#endif

size_t c16rtomb(char *UCHAR_RESTRICT s, char16_t c16, mbstate_t *UCHAR_RESTRICT ps);
size_t c32rtomb(char *UCHAR_RESTRICT s, char32_t c32, mbstate_t *UCHAR_RESTRICT ps);

size_t mbrtoc16(char16_t *UCHAR_RESTRICT pc16, const char *UCHAR_RESTRICT s, size_t n,
                mbstate_t *UCHAR_RESTRICT ps);
size_t mbrtoc32(char32_t *UCHAR_RESTRICT pc32, const char *UCHAR_RESTRICT s, size_t n,
                mbstate_t *UCHAR_RESTRICT ps);

#undef UCHAR_RESTRICT

#ifdef __cplusplus
}
#endif

#endif
