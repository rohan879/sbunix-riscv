#ifndef _TIME_H
#define _TIME_H

#include <sys/types.h>
#include <locale.h>

#ifndef __sigval_t_defined
typedef union sigval {
  int sival_int;
  void *sival_ptr;
} sigval_t;
#define __sigval_t_defined
#endif

struct sigevent;

#if !defined(__timespec_defined) && !defined(_STRUCT_TIMESPEC)
struct timespec {
    time_t tv_sec;   /* seconds */
    long   tv_nsec;  /* nanoseconds [0, 999999999] */
};
#define __timespec_defined
#endif

#if !defined(__timeval_defined) && !defined(_STRUCT_TIMEVAL)
struct timeval {
    time_t tv_sec;   /* seconds */
    long   tv_usec;  /* microseconds [0, 999999] */
};
#define __timeval_defined
#endif

struct tm {
    int tm_sec;    /* seconds [0, 60] (60 for leap second) */
    int tm_min;    /* minutes [0, 59] */
    int tm_hour;   /* hours [0, 23] */
    int tm_mday;   /* day of month [1, 31] */
    int tm_mon;    /* months since January [0, 11] */
    int tm_year;   /* years since 1900 */
    int tm_wday;   /* days since Sunday [0, 6] */
    int tm_yday;   /* days since January 1 [0, 365] */
    int tm_isdst;  /* Daylight Saving Time flag */
    long tm_gmtoff;
    const char *tm_zone;
};

#ifndef __clockid_t_defined
typedef int clockid_t;
#define __clockid_t_defined
#endif

#ifndef __timer_t_defined
typedef void *timer_t;
#define __timer_t_defined
#endif

struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};

/* Clock IDs for clock_gettime */
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3

#define TIME_UTC 1

#define SIGEV_SIGNAL 0

time_t time(time_t *t);
int    clock_gettime(int clk_id, struct timespec *tp);
int    clock_getres(clockid_t clockid, struct timespec *res);
int    clock_settime(clockid_t clockid, const struct timespec *tp);
int    clock_getcpuclockid(pid_t pid, clockid_t *clock_id);
int    nanosleep(const struct timespec *req, struct timespec *rem);
int    gettimeofday(struct timeval *tv, void *tz);

/* Timer flags */
#define TIMER_ABSTIME 1

int clock_nanosleep(int clockid, int flags,
                    const struct timespec *req, struct timespec *rem);

struct tm *gmtime(const time_t *timep);
struct tm *localtime(const time_t *timep);
struct tm *gmtime_r(const time_t *timep, struct tm *result);
struct tm *localtime_r(const time_t *timep, struct tm *result);
char *asctime(const struct tm *tm);
char *ctime(const time_t *timep);
char *asctime_r(const struct tm *tm, char *buf);
char *ctime_r(const time_t *timep, char *buf);
char *strptime(const char *s, const char *format, struct tm *tm);
void tzset(void);
struct tm *getdate(const char *string);
extern int getdate_err;
time_t     mktime(struct tm *tm);
size_t     strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
size_t     strftime_l(char *s, size_t max, const char *fmt, const struct tm *tm,
                      locale_t locale);

/* CPU time */
typedef long clock_t;
#define CLOCKS_PER_SEC 1000000L
clock_t clock(void);

/* difftime */
double difftime(time_t time1, time_t time0);

int timer_create(clockid_t clockid, struct sigevent *evp, timer_t *timerid);
int timer_delete(timer_t timerid);
int timer_getoverrun(timer_t timerid);
int timer_gettime(timer_t timerid, struct itimerspec *value);
int timer_settime(timer_t timerid, int flags,
                  const struct itimerspec *value,
                  struct itimerspec *ovalue);

int timespec_get(struct timespec *ts, int base);

extern int daylight;
extern long timezone;
extern char *tzname[2];

#endif /* _TIME_H */
