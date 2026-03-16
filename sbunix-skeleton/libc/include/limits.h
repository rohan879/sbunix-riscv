#ifndef _LIMITS_H
#define _LIMITS_H

/* Integer limits */
#define CHAR_BIT    8
#define CHAR_MIN    (-128)
#define CHAR_MAX    127
#define UCHAR_MAX   255U
#define SCHAR_MIN   (-128)
#define SCHAR_MAX   127

#define SHRT_MIN    (-32768)
#define SHRT_MAX    32767
#define USHRT_MAX   65535U

#define INT_MIN     (-2147483648)
#define INT_MAX     2147483647
#define UINT_MAX    4294967295U

#define LONG_MAX    0x7fffffffffffffffL
#define LONG_MIN    (-LONG_MAX - 1L)
#define ULONG_MAX   0xffffffffffffffffUL

#define LLONG_MAX   0x7fffffffffffffffLL
#define LLONG_MIN   (-LLONG_MAX - 1LL)
#define ULLONG_MAX  0xffffffffffffffffULL

/* POSIX path and name limits */
#define PATH_MAX    4096  /* maximum bytes in a path name */
#define NAME_MAX    255   /* maximum bytes in a filename component */
#define PIPE_BUF    4096  /* maximum bytes for atomic pipe write */

/* POSIX open files / process limits */
#define OPEN_MAX    64    /* maximum open files per process */
#define NGROUPS_MAX 32    /* maximum supplementary group IDs */
#define ARG_MAX     65536 /* maximum bytes of exec arguments */

/* POSIX ssize_t limit */
#define SSIZE_MAX   LONG_MAX

#endif /* _LIMITS_H */
