/*
 * time.c — time-related syscall wrappers for SBUnix libc.
 */

#include <time.h>
#include <syscall.h>
#include <errno.h>

/*
 * nanosleep() — sleep for at least the duration specified by *req.
 * If interrupted, the remaining time is written to *rem (if non-NULL).
 */
int nanosleep(const struct timespec *req, struct timespec *rem) {
    return (int)syscall(SYS_nanosleep, req, rem);
}

/*
 * clock_gettime() — read a POSIX clock.
 * On SBUnix the kernel only handles CLOCK_REALTIME / CLOCK_MONOTONIC
 * via SYS_clock_gettime (113).  Returns 0 on success, -1 on error.
 */
int clock_gettime(int clk_id, struct timespec *tp) {
    return (int)__syscall_ret(syscall(SYS_clock_gettime, clk_id, tp));
}

/*
 * time() — return seconds since the Unix epoch.
 * Uses clock_gettime(CLOCK_REALTIME) as the underlying mechanism.
 * If t is non-NULL, the result is also stored in *t.
 */
time_t time(time_t *t) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
        return (time_t)-1;
    if (t)
        *t = ts.tv_sec;
    return ts.tv_sec;
}

/*
 * gettimeofday() — get time of day with microsecond resolution.
 * Wraps clock_gettime(CLOCK_REALTIME) and converts to struct timeval.
 */
int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv)
        return -1;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
        return -1;
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

int clock_nanosleep(int clockid, int flags,
                    const struct timespec *req, struct timespec *rem) {
    long ret = syscall(SYS_clock_nanosleep, clockid, flags, req, rem);
    if (ret < 0) { errno = (int)(-ret); return (int)(-ret); }
    return 0;
}

/* -----------------------------------------------------------------------
 * Time conversion utilities
 * ---------------------------------------------------------------------- */

#include <string.h>
#include <stddef.h>

/* Days per month for non-leap and leap years */
static const int days_in_month[2][12] = {
    { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },  /* normal */
    { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },  /* leap   */
};

static const char *wday_name[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *mon_name[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static int is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/*
 * gmtime() — convert time_t to struct tm in UTC.
 * Uses a static buffer (not thread-safe, but SBUnix is single-threaded).
 */
struct tm *gmtime(const time_t *timer) {
    static struct tm result;
    time_t t = *timer;

    result.tm_sec  = (int)(t % 60);
    t /= 60;
    result.tm_min  = (int)(t % 60);
    t /= 60;
    result.tm_hour = (int)(t % 24);

    /* Days since epoch (Jan 1, 1970) */
    long days = (long)((*timer) / SECS_PER_DAY);

    /* Day of week: Jan 1, 1970 was a Thursday (4) */
    result.tm_wday = (int)((days + 4) % 7);
    if (result.tm_wday < 0)
        result.tm_wday += 7;

    /* Compute year, month, day */
    int year = 1970;
    while (1) {
        int yday_max = is_leap_year(year) ? 366 : 365;
        if (days < yday_max)
            break;
        days -= yday_max;
        year++;
    }

    result.tm_year = year - 1900;
    result.tm_yday = (int)days;
    result.tm_isdst = 0;

    int leap = is_leap_year(year);
    int mon;
    for (mon = 0; mon < 11; mon++) {
        if (days < days_in_month[leap][mon])
            break;
        days -= days_in_month[leap][mon];
    }
    result.tm_mon  = mon;
    result.tm_mday = (int)days + 1;

    return &result;
}

/*
 * localtime() — same as gmtime (no timezone support in SBUnix).
 */
struct tm *localtime(const time_t *timer) {
    return gmtime(timer);
}

/*
 * mktime() — convert struct tm to time_t (assumes UTC).
 * Also normalizes the struct tm fields.
 */
time_t mktime(struct tm *tm) {
    int year = tm->tm_year + 1900;
    int mon  = tm->tm_mon;
    int mday = tm->tm_mday;

    /* Normalize month */
    while (mon < 0)  { mon += 12; year--; }
    while (mon > 11) { mon -= 12; year++; }

    /* Count days from epoch to start of this year */
    long days = 0;
    if (year >= 1970) {
        for (int y = 1970; y < year; y++)
            days += is_leap_year(y) ? 366 : 365;
    } else {
        for (int y = year; y < 1970; y++)
            days -= is_leap_year(y) ? 366 : 365;
    }

    /* Add days for months */
    int leap = is_leap_year(year);
    for (int m = 0; m < mon; m++)
        days += days_in_month[leap][m];

    /* Add day of month (1-based) */
    days += mday - 1;

    time_t result = (time_t)days * SECS_PER_DAY
                  + (time_t)tm->tm_hour * SECS_PER_HOUR
                  + (time_t)tm->tm_min  * SECS_PER_MIN
                  + (time_t)tm->tm_sec;

    /* Normalize the struct tm */
    struct tm *normalized = gmtime(&result);
    *tm = *normalized;

    return result;
}

/*
 * difftime() — return t1 - t0 as a long (no FPU, so we avoid double).
 */
long difftime(time_t t1, time_t t0) {
    return (long)(t1 - t0);
}

/* Helper: write a zero-padded decimal number into buf */
static int fmt_num(char *buf, size_t remaining, int val, int width) {
    char tmp[16];
    int  len = 0;
    int  neg = 0;
    unsigned int uval;

    if (val < 0) { neg = 1; uval = (unsigned int)(-val); }
    else         { uval = (unsigned int)val; }

    do {
        tmp[len++] = '0' + (char)(uval % 10);
        uval /= 10;
    } while (uval > 0);

    /* Pad with zeros */
    while (len < width)
        tmp[len++] = '0';
    if (neg)
        tmp[len++] = '-';

    if ((size_t)len > remaining)
        return 0;

    /* Reverse into buf */
    for (int i = 0; i < len; i++)
        buf[i] = tmp[len - 1 - i];
    return len;
}

/* Helper: copy a string into buf */
static int fmt_str(char *buf, size_t remaining, const char *s) {
    int len = 0;
    while (s[len]) len++;
    if ((size_t)len > remaining) return 0;
    for (int i = 0; i < len; i++) buf[i] = s[i];
    return len;
}

/*
 * strftime() — format a time string.
 * Supports: %Y %m %d %H %M %S %j %w %a %b %c %%
 */
size_t strftime(char *s, size_t max, const char *format,
                const struct tm *tm) {
    size_t pos = 0;

    for (const char *f = format; *f && pos < max - 1; f++) {
        if (*f != '%') {
            s[pos++] = *f;
            continue;
        }
        f++;
        if (!*f) break;

        int n = 0;
        size_t rem = max - 1 - pos;

        switch (*f) {
        case 'Y': /* 4-digit year */
            n = fmt_num(s + pos, rem, tm->tm_year + 1900, 4);
            break;
        case 'm': /* month 01-12 */
            n = fmt_num(s + pos, rem, tm->tm_mon + 1, 2);
            break;
        case 'd': /* day of month 01-31 */
            n = fmt_num(s + pos, rem, tm->tm_mday, 2);
            break;
        case 'H': /* hour 00-23 */
            n = fmt_num(s + pos, rem, tm->tm_hour, 2);
            break;
        case 'M': /* minute 00-59 */
            n = fmt_num(s + pos, rem, tm->tm_min, 2);
            break;
        case 'S': /* second 00-60 */
            n = fmt_num(s + pos, rem, tm->tm_sec, 2);
            break;
        case 'j': /* day of year 001-366 */
            n = fmt_num(s + pos, rem, tm->tm_yday + 1, 3);
            break;
        case 'w': /* weekday 0-6 */
            n = fmt_num(s + pos, rem, tm->tm_wday, 1);
            break;
        case 'a': /* abbreviated weekday name */
            if (tm->tm_wday >= 0 && tm->tm_wday <= 6)
                n = fmt_str(s + pos, rem, wday_name[tm->tm_wday]);
            break;
        case 'b': /* abbreviated month name */
        case 'h': /* same as %b */
            if (tm->tm_mon >= 0 && tm->tm_mon <= 11)
                n = fmt_str(s + pos, rem, mon_name[tm->tm_mon]);
            break;
        case 'c': { /* date and time: "Sun Jan  1 00:00:00 1970" */
            char tmp[64];
            int tlen = 0;
            if (tm->tm_wday >= 0 && tm->tm_wday <= 6 &&
                tm->tm_mon >= 0 && tm->tm_mon <= 11) {
                /* Build "Www Mmm DD HH:MM:SS YYYY" */
                tlen += fmt_str(tmp + tlen, sizeof(tmp) - (size_t)tlen,
                                wday_name[tm->tm_wday]);
                tmp[tlen++] = ' ';
                tlen += fmt_str(tmp + tlen, sizeof(tmp) - (size_t)tlen,
                                mon_name[tm->tm_mon]);
                tmp[tlen++] = ' ';
                tlen += fmt_num(tmp + tlen, sizeof(tmp) - (size_t)tlen,
                                tm->tm_mday, 2);
                tmp[tlen++] = ' ';
                tlen += fmt_num(tmp + tlen, sizeof(tmp) - (size_t)tlen,
                                tm->tm_hour, 2);
                tmp[tlen++] = ':';
                tlen += fmt_num(tmp + tlen, sizeof(tmp) - (size_t)tlen,
                                tm->tm_min, 2);
                tmp[tlen++] = ':';
                tlen += fmt_num(tmp + tlen, sizeof(tmp) - (size_t)tlen,
                                tm->tm_sec, 2);
                tmp[tlen++] = ' ';
                tlen += fmt_num(tmp + tlen, sizeof(tmp) - (size_t)tlen,
                                tm->tm_year + 1900, 4);
            }
            if ((size_t)tlen <= rem) {
                for (int i = 0; i < tlen; i++)
                    s[pos + i] = tmp[i];
                n = tlen;
            }
            break;
        }
        case '%':
            if (rem >= 1) { s[pos] = '%'; n = 1; }
            break;
        default:
            /* Unknown specifier — copy literally */
            if (rem >= 2) { s[pos] = '%'; s[pos+1] = *f; n = 2; }
            break;
        }
        pos += (size_t)n;
    }

    s[pos] = '\0';
    return pos;
}
