#ifndef _STDINT_H
#define _STDINT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;

typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long        int64_t;

typedef unsigned long      uintptr_t;
typedef long               intptr_t;
typedef unsigned long      uintmax_t;
typedef long               intmax_t;

typedef int                int_least8_t;
typedef int                int_least16_t;
typedef int                int_least32_t;
typedef long               int_least64_t;

typedef unsigned int       uint_least8_t;
typedef unsigned int       uint_least16_t;
typedef unsigned int       uint_least32_t;
typedef unsigned long      uint_least64_t;

typedef int                int_fast8_t;
typedef long               int_fast16_t;
typedef long               int_fast32_t;
typedef long               int_fast64_t;

typedef unsigned int       uint_fast8_t;
typedef unsigned long      uint_fast16_t;
typedef unsigned long      uint_fast32_t;
typedef unsigned long      uint_fast64_t;

#define INT8_MIN   (-128)
#define INT8_MAX   (127)
#define INT16_MIN  (-32768)
#define INT16_MAX  (32767)
#define INT32_MIN  (-2147483648)
#define INT32_MAX  (2147483647)
#define INT64_MIN  (-9223372036854775808L)
#define INT64_MAX  (9223372036854775807L)

#define UINT8_MAX  (255U)
#define UINT16_MAX (65535U)
#define UINT32_MAX (4294967295U)
#define UINT64_MAX (18446744073709551615UL)

#define INTPTR_MIN  INT64_MIN
#define INTPTR_MAX  INT64_MAX
#define UINTPTR_MAX UINT64_MAX

#define SIZE_MAX    UINT64_MAX

#define INT_LEAST8_MIN    INT8_MIN
#define INT_LEAST8_MAX    INT8_MAX
#define UINT_LEAST8_MAX   UINT8_MAX
#define INT_LEAST16_MIN   INT16_MIN
#define INT_LEAST16_MAX   INT16_MAX
#define UINT_LEAST16_MAX  UINT16_MAX
#define INT_LEAST32_MIN   INT32_MIN
#define INT_LEAST32_MAX   INT32_MAX
#define UINT_LEAST32_MAX  UINT32_MAX
#define INT_LEAST64_MIN   INT64_MIN
#define INT_LEAST64_MAX   INT64_MAX
#define UINT_LEAST64_MAX  UINT64_MAX

#define INT_FAST8_MIN     INT32_MIN
#define INT_FAST8_MAX     INT32_MAX
#define UINT_FAST8_MAX    UINT32_MAX
#define INT_FAST16_MIN    INT64_MIN
#define INT_FAST16_MAX    INT64_MAX
#define UINT_FAST16_MAX   UINT64_MAX
#define INT_FAST32_MIN    INT64_MIN
#define INT_FAST32_MAX    INT64_MAX
#define UINT_FAST32_MAX   UINT64_MAX
#define INT_FAST64_MIN    INT64_MIN
#define INT_FAST64_MAX    INT64_MAX
#define UINT_FAST64_MAX   UINT64_MAX

#define INTMAX_MIN        INT64_MIN
#define INTMAX_MAX        INT64_MAX
#define UINTMAX_MAX       UINT64_MAX

#define PTRDIFF_MIN       LONG_MIN
#define PTRDIFF_MAX       LONG_MAX

#define SIG_ATOMIC_MIN    INT_MIN
#define SIG_ATOMIC_MAX    INT_MAX

#define WCHAR_MIN         INT_MIN
#define WCHAR_MAX         INT_MAX
#define WINT_MIN          0
#define WINT_MAX          UINT_MAX

#define INT8_C(x)   (x)
#define INT16_C(x)  (x)
#define INT32_C(x)  (x)
#define INT64_C(x)  (x##L)

#define UINT8_C(x)  (x##U)
#define UINT16_C(x) (x##U)
#define UINT32_C(x) (x##U)
#define UINT64_C(x) (x##UL)

#define INTMAX_C(x) INT64_C(x)
#define UINTMAX_C(x) UINT64_C(x)

#ifdef __cplusplus
}
#endif

#endif /* _STDINT_H */
