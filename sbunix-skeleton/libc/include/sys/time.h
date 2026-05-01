#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <sys/select.h>
#include <time.h>      /* struct timeval, struct timespec */
#include <sys/types.h>

/* Historical BSD timezone struct used by settimeofday(). */
struct timezone {
  int tz_minuteswest;
  int tz_dsttime;
};

/* gettimeofday() */
int gettimeofday(struct timeval *tv, struct timezone *tz);
int settimeofday(const struct timeval *tv, const struct timezone *tz);
int utimes(const char *path, const struct timeval times[2]);

struct itimerval {
  struct timeval it_interval;
  struct timeval it_value;
};

#define ITIMER_REAL 0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF 2

int getitimer(int which, struct itimerval *curr_value);
int setitimer(int which, const struct itimerval *new_value,
              struct itimerval *old_value);

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
