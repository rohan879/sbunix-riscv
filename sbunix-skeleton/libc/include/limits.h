#ifndef _LIMITS_H
#define _LIMITS_H

#ifdef __cplusplus
extern "C" {
#endif

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

#define LONG_BIT    64
#define WORD_BIT    32

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

#define MB_LEN_MAX 16

/* Common implementation limits used by include/limits tests */
#define AIO_LISTIO_MAX 64
#define AIO_MAX 1024
#define AIO_PRIO_DELTA_MAX 20
#define ATEXIT_MAX 32
#define BC_BASE_MAX 99
#define BC_DIM_MAX 2048
#define BC_SCALE_MAX 99
#define BC_STRING_MAX 1000
#define CHARCLASS_NAME_MAX 14
#define CHILD_MAX 999
#define COLL_WEIGHTS_MAX 2
#define DELAYTIMER_MAX 2147483647
#define EXPR_NEST_MAX 32
#define FILESIZEBITS 64
#define GETENTROPY_MAX 256
#define HOST_NAME_MAX 255
#define IOV_MAX 1024
#define LINE_MAX 2048
#define LINK_MAX 127
#define LOGIN_NAME_MAX 256
#define MAX_CANON 255
#define MAX_INPUT 255
#define MQ_OPEN_MAX 64
#define MQ_PRIO_MAX 32768
#define NL_ARGMAX 9
#define NL_LANGMAX 2048
#define NL_MSGMAX 32767
#define NL_SETMAX 255
#define NL_TEXTMAX 2048
#define NSIG_MAX 65
#define NZERO 20
#define PAGE_SIZE 4096
#define PAGESIZE PAGE_SIZE
#define POSIX_ALLOC_SIZE_MIN 0
#define POSIX_REC_INCR_XFER_SIZE 4096
#define POSIX_REC_MAX_XFER_SIZE 1048576
#define POSIX_REC_MIN_XFER_SIZE 4096
#define POSIX_REC_XFER_ALIGN 4096
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#define PTHREAD_KEYS_MAX 128
#define PTHREAD_STACK_MIN 16384
#define PTHREAD_THREADS_MAX 1024
#define RE_DUP_MAX 255
#define RTSIG_MAX 32
#define SEM_NSEMS_MAX 256
#define SEM_VALUE_MAX 32767
#define SIGQUEUE_MAX 256
#define SS_REPL_MAX 4
#define STREAM_MAX 16
#define SYMLINK_MAX 255
#define SYMLOOP_MAX 40
#define TEXTDOMAIN_MAX 2048
#define TIMER_MAX 32
#define TTY_NAME_MAX 32
#define TZNAME_MAX 6
#define _XOPEN_IOV_MAX IOV_MAX
#define _XOPEN_NAME_MAX NAME_MAX
#define _XOPEN_PATH_MAX PATH_MAX

/* POSIX minimum values */
#define _POSIX2_BC_BASE_MAX 99
#define _POSIX2_BC_DIM_MAX 2048
#define _POSIX2_BC_SCALE_MAX 99
#define _POSIX2_BC_STRING_MAX 1000
#define _POSIX2_CHARCLASS_NAME_MAX 14
#define _POSIX2_COLL_WEIGHTS_MAX 2
#define _POSIX2_EXPR_NEST_MAX 32
#define _POSIX2_LINE_MAX 2048
#define _POSIX2_RE_DUP_MAX 255

#define _POSIX_AIO_LISTIO_MAX 2
#define _POSIX_AIO_MAX 1
#define _POSIX_ARG_MAX 4096
#define _POSIX_CHILD_MAX 25
#define _POSIX_CLOCKRES_MIN 20000000
#define _POSIX_DELAYTIMER_MAX 32
#define _POSIX_HOST_NAME_MAX 255
#define _POSIX_LINK_MAX 8
#define _POSIX_LOGIN_NAME_MAX 9
#define _POSIX_MAX_CANON 255
#define _POSIX_MAX_INPUT 255
#define _POSIX_MQ_OPEN_MAX 8
#define _POSIX_MQ_PRIO_MAX 32
#define _POSIX_NAME_MAX 14
#define _POSIX_NGROUPS_MAX 8
#define _POSIX_OPEN_MAX 20
#define _POSIX_PATH_MAX 256
#define _POSIX_PIPE_BUF 512
#define _POSIX_RE_DUP_MAX 255
#define _POSIX_RTSIG_MAX 8
#define _POSIX_SEM_NSEMS_MAX 256
#define _POSIX_SEM_VALUE_MAX 32767
#define _POSIX_SIGQUEUE_MAX 32
#define _POSIX_SSIZE_MAX 32767
#define _POSIX_SS_REPL_MAX 4
#define _POSIX_STREAM_MAX 8
#define _POSIX_SYMLINK_MAX 255
#define _POSIX_SYMLOOP_MAX 8
#define _POSIX_THREAD_DESTRUCTOR_ITERATIONS 4
#define _POSIX_THREAD_KEYS_MAX 128
#define _POSIX_THREAD_THREADS_MAX 64
#define _POSIX_TIMER_MAX 32
#define _POSIX_TTY_NAME_MAX 9
#define _POSIX_TZNAME_MAX 6

#ifdef __cplusplus
}
#endif

#endif /* _LIMITS_H */
