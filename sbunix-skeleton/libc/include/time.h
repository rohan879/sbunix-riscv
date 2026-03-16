#ifndef _TIME_H
#define _TIME_H

#include <sys/types.h>

struct timespec {
    time_t tv_sec;   /* seconds */
    long   tv_nsec;  /* nanoseconds [0, 999999999] */
};

struct timeval {
    time_t tv_sec;   /* seconds */
    long   tv_usec;  /* microseconds [0, 999999] */
};

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
};

/* Clock IDs for clock_gettime */
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3

time_t time(time_t *t);
int    clock_gettime(int clk_id, struct timespec *tp);
int    nanosleep(const struct timespec *req, struct timespec *rem);
int    gettimeofday(struct timeval *tv, void *tz);

/* Timer flags */
#define TIMER_ABSTIME 1

int clock_nanosleep(int clockid, int flags,
                    const struct timespec *req, struct timespec *rem);

struct tm *gmtime(const time_t *timep);
struct tm *localtime(const time_t *timep);
time_t     mktime(struct tm *tm);
size_t     strftime(char *s, size_t max, const char *fmt, const struct tm *tm);

/* CPU time */
typedef long clock_t;
#define CLOCKS_PER_SEC 1000000L
clock_t clock(void);

/* difftime */
double difftime(time_t time1, time_t time0);

#endif /* _TIME_H */
