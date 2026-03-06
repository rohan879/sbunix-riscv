#ifndef _STDINT_H
#define _STDINT_H

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

#define INT8_C(x)   (x)
#define INT16_C(x)  (x)
#define INT32_C(x)  (x)
#define INT64_C(x)  (x##L)

#define UINT8_C(x)  (x##U)
#define UINT16_C(x) (x##U)
#define UINT32_C(x) (x##U)
#define UINT64_C(x) (x##UL)

#endif /* _STDINT_H */
