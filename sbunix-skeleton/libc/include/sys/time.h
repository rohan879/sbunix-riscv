#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <time.h>      /* struct timeval, struct timespec */
#include <sys/types.h>

/* gettimeofday() */
int gettimeofday(struct timeval *tv, void *tz);

/* Timer arithmetic macros */
#define timersub(a, b, result) \
  do { \
    (result)->tv_sec  = (a)->tv_sec  - (b)->tv_sec; \
    (result)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
    if ((result)->tv_usec < 0) { \
      --(result)->tv_sec; \
      (result)->tv_usec += 1000000; \
    } \
  } while (0)

#define timeradd(a, b, result) \
  do { \
    (result)->tv_sec  = (a)->tv_sec  + (b)->tv_sec; \
    (result)->tv_usec = (a)->tv_usec + (b)->tv_usec; \
    if ((result)->tv_usec >= 1000000) { \
      ++(result)->tv_sec; \
      (result)->tv_usec -= 1000000; \
    } \
  } while (0)

#define timercmp(a, b, CMP) \
  (((a)->tv_sec == (b)->tv_sec) \
    ? ((a)->tv_usec CMP (b)->tv_usec) \
    : ((a)->tv_sec CMP (b)->tv_sec))

#define timerclear(tvp) ((tvp)->tv_sec = (tvp)->tv_usec = 0)
#define timerisset(tvp) ((tvp)->tv_sec || (tvp)->tv_usec)

#endif /* _SYS_TIME_H */
