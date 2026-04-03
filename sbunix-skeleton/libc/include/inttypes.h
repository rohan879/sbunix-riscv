#ifndef _INTTYPES_H
#define _INTTYPES_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
  intmax_t quot;
  intmax_t rem;
} imaxdiv_t;

intmax_t imaxabs(intmax_t j);
imaxdiv_t imaxdiv(intmax_t numer, intmax_t denom);
intmax_t strtoimax(const char *nptr, char **endptr, int base);
uintmax_t strtoumax(const char *nptr, char **endptr, int base);
intmax_t wcstoimax(const wchar_t *nptr, wchar_t **endptr, int base);
uintmax_t wcstoumax(const wchar_t *nptr, wchar_t **endptr, int base);

#define PRId8 "hhd"
#define PRIi8 "hhi"
#define PRIo8 "hho"
#define PRIu8 "hhu"
#define PRIx8 "hhx"
#define PRIX8 "hhX"

#define PRId16 "hd"
#define PRIi16 "hi"
#define PRIo16 "ho"
#define PRIu16 "hu"
#define PRIx16 "hx"
#define PRIX16 "hX"

#define PRId32 "d"
#define PRIi32 "i"
#define PRIo32 "o"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"

#define PRId64 "ld"
#define PRIi64 "li"
#define PRIo64 "lo"
#define PRIu64 "lu"
#define PRIx64 "lx"
#define PRIX64 "lX"

#define PRIdLEAST8 PRId8
#define PRIiLEAST8 PRIi8
#define PRIoLEAST8 PRIo8
#define PRIuLEAST8 PRIu8
#define PRIxLEAST8 PRIx8
#define PRIXLEAST8 PRIX8

#define PRIdLEAST16 PRId16
#define PRIiLEAST16 PRIi16
#define PRIoLEAST16 PRIo16
#define PRIuLEAST16 PRIu16
#define PRIxLEAST16 PRIx16
#define PRIXLEAST16 PRIX16

#define PRIdLEAST32 PRId32
#define PRIiLEAST32 PRIi32
#define PRIoLEAST32 PRIo32
#define PRIuLEAST32 PRIu32
#define PRIxLEAST32 PRIx32
#define PRIXLEAST32 PRIX32

#define PRIdLEAST64 PRId64
#define PRIiLEAST64 PRIi64
#define PRIoLEAST64 PRIo64
#define PRIuLEAST64 PRIu64
#define PRIxLEAST64 PRIx64
#define PRIXLEAST64 PRIX64

#define PRIdFAST8 "d"
#define PRIiFAST8 "i"
#define PRIoFAST8 "o"
#define PRIuFAST8 "u"
#define PRIxFAST8 "x"
#define PRIXFAST8 "X"

#define PRIdFAST16 "ld"
#define PRIiFAST16 "li"
#define PRIoFAST16 "lo"
#define PRIuFAST16 "lu"
#define PRIxFAST16 "lx"
#define PRIXFAST16 "lX"

#define PRIdFAST32 "ld"
#define PRIiFAST32 "li"
#define PRIoFAST32 "lo"
#define PRIuFAST32 "lu"
#define PRIxFAST32 "lx"
#define PRIXFAST32 "lX"

#define PRIdFAST64 "ld"
#define PRIiFAST64 "li"
#define PRIoFAST64 "lo"
#define PRIuFAST64 "lu"
#define PRIxFAST64 "lx"
#define PRIXFAST64 "lX"

#define PRIdMAX "ld"
#define PRIiMAX "li"
#define PRIoMAX "lo"
#define PRIuMAX "lu"
#define PRIxMAX "lx"
#define PRIXMAX "lX"

#define PRIdPTR "ld"
#define PRIiPTR "li"
#define PRIoPTR "lo"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#define PRIXPTR "lX"

#define SCNd8 "hhd"
#define SCNi8 "hhi"
#define SCNo8 "hho"
#define SCNu8 "hhu"
#define SCNx8 "hhx"

#define SCNd16 "hd"
#define SCNi16 "hi"
#define SCNo16 "ho"
#define SCNu16 "hu"
#define SCNx16 "hx"

#define SCNd32 "d"
#define SCNi32 "i"
#define SCNo32 "o"
#define SCNu32 "u"
#define SCNx32 "x"

#define SCNd64 "ld"
#define SCNi64 "li"
#define SCNo64 "lo"
#define SCNu64 "lu"
#define SCNx64 "lx"

#define SCNdLEAST8 SCNd8
#define SCNiLEAST8 SCNi8
#define SCNoLEAST8 SCNo8
#define SCNuLEAST8 SCNu8
#define SCNxLEAST8 SCNx8

#define SCNdLEAST16 SCNd16
#define SCNiLEAST16 SCNi16
#define SCNoLEAST16 SCNo16
#define SCNuLEAST16 SCNu16
#define SCNxLEAST16 SCNx16

#define SCNdLEAST32 SCNd32
#define SCNiLEAST32 SCNi32
#define SCNoLEAST32 SCNo32
#define SCNuLEAST32 SCNu32
#define SCNxLEAST32 SCNx32

#define SCNdLEAST64 SCNd64
#define SCNiLEAST64 SCNi64
#define SCNoLEAST64 SCNo64
#define SCNuLEAST64 SCNu64
#define SCNxLEAST64 SCNx64

#define SCNdFAST8 "d"
#define SCNiFAST8 "i"
#define SCNoFAST8 "o"
#define SCNuFAST8 "u"
#define SCNxFAST8 "x"

#define SCNdFAST16 "ld"
#define SCNiFAST16 "li"
#define SCNoFAST16 "lo"
#define SCNuFAST16 "lu"
#define SCNxFAST16 "lx"

#define SCNdFAST32 "ld"
#define SCNiFAST32 "li"
#define SCNoFAST32 "lo"
#define SCNuFAST32 "lu"
#define SCNxFAST32 "lx"

#define SCNdFAST64 "ld"
#define SCNiFAST64 "li"
#define SCNoFAST64 "lo"
#define SCNuFAST64 "lu"
#define SCNxFAST64 "lx"

#define SCNdMAX "ld"
#define SCNiMAX "li"
#define SCNoMAX "lo"
#define SCNuMAX "lu"
#define SCNxMAX "lx"

#define SCNdPTR "ld"
#define SCNiPTR "li"
#define SCNoPTR "lo"
#define SCNuPTR "lu"
#define SCNxPTR "lx"

#endif
