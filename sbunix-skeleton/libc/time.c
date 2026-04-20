/*
 * time.c — time-related syscall wrappers and calendar functions for SBUnix libc.
 */

#include <time.h>
#include <sys/time.h>
#include <syscall.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>

long syscall(long num, ...);

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static int _is_leap(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int _days_in_year(int year)
{
    return _is_leap(year) ? 366 : 365;
}

static const int _mon_days[2][12] = {
    /* non-leap */ {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    /* leap     */ {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
};

static const char *_wday_abbr[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

/* ENH: Full weekday names needed for %A strftime specifier */
static const char *_wday_full[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"
};

static const char *_mon_abbr[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* ENH: Full month names needed for %B strftime specifier */
static const char *_mon_full[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

int getdate_err;
int daylight = 0;
long timezone = 0;
char *tzname[2] = {"UTC", "UTC"};

/*
 * _append — append src to buf at *pos, respecting max.
 * Returns number of characters written (excluding NUL).
 */
static size_t _append(char *buf, size_t max, size_t *pos, const char *src)
{
    size_t n = 0;
    while (*src && *pos + 1 < max) {
        buf[(*pos)++] = *src++;
        n++;
    }
    return n;
}

/*
 * _append_num — append a zero-padded decimal number of exactly `width` digits.
 */
static size_t _append_num(char *buf, size_t max, size_t *pos, int val, int width)
{
    char tmp[16];
    int  i = 0;
    int  neg = 0;

    if (val < 0) { neg = 1; val = -val; }

    /* generate digits in reverse */
    do {
        tmp[i++] = (char)('0' + val % 10);
        val /= 10;
    } while (val);

    /* pad with zeros to desired width (after sign) */
    while (i < width - neg)
        tmp[i++] = '0';
    if (neg)
        tmp[i++] = '-';

    /* reverse into buf */
    size_t n = 0;
    while (i > 0 && *pos + 1 < max) {
        buf[(*pos)++] = tmp[--i];
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  gmtime                                                             */
/* ------------------------------------------------------------------ */

struct tm *gmtime(const time_t *timep)
{
    static struct tm result;
    time_t t;
    int    year, mon, leap;

    if (!timep)
        return (struct tm *)0;

    t = *timep;

    /* seconds, minutes, hours */
    result.tm_sec  = (int)(t % 60);  t /= 60;
    result.tm_min  = (int)(t % 60);  t /= 60;
    result.tm_hour = (int)(t % 24);

    /* total days since epoch (Jan 1 1970 was a Thursday → wday 4) */
    long days = (long)(t / 24);

    result.tm_wday = (int)((days + 4) % 7);          /* Thu = 4 */
    if (result.tm_wday < 0)
        result.tm_wday += 7;

    /* walk years from 1970 */
    year = 1970;
    if (days >= 0) {
        while (days >= _days_in_year(year)) {
            days -= _days_in_year(year);
            year++;
        }
    } else {
        while (days < 0) {
            year--;
            days += _days_in_year(year);
        }
    }

    result.tm_year = year - 1900;
    result.tm_yday = (int)days;

    /* walk months */
    leap = _is_leap(year);
    for (mon = 0; mon < 11; mon++) {
        if (days < _mon_days[leap][mon])
            break;
        days -= _mon_days[leap][mon];
    }
    result.tm_mon  = mon;
    result.tm_mday = (int)days + 1;
    result.tm_isdst = 0;
    result.tm_gmtoff = 0;
    result.tm_zone = "UTC";

    return &result;
}

/* ------------------------------------------------------------------ */
/*  localtime  — same as gmtime (no timezone support)                  */
/* ------------------------------------------------------------------ */

struct tm *localtime(const time_t *timep)
{
    return gmtime(timep);
}

struct tm *gmtime_r(const time_t *timep, struct tm *result)
{
    struct tm *tmp = gmtime(timep);
    if (!tmp || !result)
        return (struct tm *)0;
    *result = *tmp;
    return result;
}

struct tm *localtime_r(const time_t *timep, struct tm *result)
{
    struct tm *tmp = localtime(timep);
    if (!tmp || !result)
        return (struct tm *)0;
    *result = *tmp;
    return result;
}

char *asctime(const struct tm *tm)
{
    static char buf[32];
    if (!tm)
        return (char *)0;
    snprintf(buf, sizeof(buf), "%s %s %2d %02d:%02d:%02d %d\n",
             _wday_abbr[tm->tm_wday < 0 ? 0 : tm->tm_wday % 7],
             _mon_abbr[tm->tm_mon < 0 ? 0 : tm->tm_mon % 12],
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
             tm->tm_year + 1900);
    return buf;
}

char *asctime_r(const struct tm *tm, char *buf)
{
    if (!tm || !buf)
        return (char *)0;
    snprintf(buf, 26, "%s %s %2d %02d:%02d:%02d %d\n",
             _wday_abbr[tm->tm_wday < 0 ? 0 : tm->tm_wday % 7],
             _mon_abbr[tm->tm_mon < 0 ? 0 : tm->tm_mon % 12],
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
             tm->tm_year + 1900);
    return buf;
}

char *ctime(const time_t *timep)
{
    struct tm *tm = localtime(timep);
    if (!tm)
        return (char *)0;
    return asctime(tm);
}

char *ctime_r(const time_t *timep, char *buf)
{
    struct tm tmp;
    if (!timep || !buf)
        return (char *)0;
    if (!localtime_r(timep, &tmp))
        return (char *)0;
    return asctime_r(&tmp, buf);
}

/*
 * strptime — parse date/time string according to format.
 *
 * Supports core POSIX specifiers: %Y, %m, %d, %H, %M, %S, %j, %n, %t, %%.
 * Returns pointer to first unconsumed character, or NULL on mismatch.
 */
char *strptime(const char *s, const char *format, struct tm *tm)
{
    if (!s || !format || !tm)
        return (char *)0;

    while (*format) {
        if (*format == '%') {
            format++;
            switch (*format) {
            case 'Y': {
                /* 4-digit year — parse up to 4 digits */
                int val = 0;
                int digits = 0;
                while (*s >= '0' && *s <= '9' && digits < 4) {
                    val = val * 10 + (*s - '0');
                    s++;
                    digits++;
                }
                if (digits == 0)
                    return (char *)0;
                /* tm_year is years since 1900 */
                tm->tm_year = val - 1900;
                break;
            }
            case 'm': {
                /* Month [01,12] */
                int val = 0;
                int digits = 0;
                /* Skip leading whitespace for %m per POSIX */
                while (*s == ' ')
                    s++;
                while (*s >= '0' && *s <= '9' && digits < 2) {
                    val = val * 10 + (*s - '0');
                    s++;
                    digits++;
                }
                if (digits == 0 || val < 1 || val > 12)
                    return (char *)0;
                /* tm_mon is 0-based */
                tm->tm_mon = val - 1;
                break;
            }
            case 'd': {
                /* Day of month [01,31] */
                int val = 0;
                int digits = 0;
                while (*s == ' ')
                    s++;
                while (*s >= '0' && *s <= '9' && digits < 2) {
                    val = val * 10 + (*s - '0');
                    s++;
                    digits++;
                }
                if (digits == 0 || val < 1 || val > 31)
                    return (char *)0;
                tm->tm_mday = val;
                break;
            }
            case 'H': {
                /* Hour [00,23] */
                int val = 0;
                int digits = 0;
                while (*s == ' ')
                    s++;
                while (*s >= '0' && *s <= '9' && digits < 2) {
                    val = val * 10 + (*s - '0');
                    s++;
                    digits++;
                }
                if (digits == 0 || val > 23)
                    return (char *)0;
                tm->tm_hour = val;
                break;
            }
            case 'M': {
                /* Minute [00,59] */
                int val = 0;
                int digits = 0;
                while (*s == ' ')
                    s++;
                while (*s >= '0' && *s <= '9' && digits < 2) {
                    val = val * 10 + (*s - '0');
                    s++;
                    digits++;
                }
                if (digits == 0 || val > 59)
                    return (char *)0;
                tm->tm_min = val;
                break;
            }
            case 'S': {
                /* Second [00,60] (60 for leap second) */
                int val = 0;
                int digits = 0;
                while (*s == ' ')
                    s++;
                while (*s >= '0' && *s <= '9' && digits < 2) {
                    val = val * 10 + (*s - '0');
                    s++;
                    digits++;
                }
                if (digits == 0 || val > 60)
                    return (char *)0;
                tm->tm_sec = val;
                break;
            }
            case 'j': {
                /* Day of year [001,366] */
                int val = 0;
                int digits = 0;
                while (*s >= '0' && *s <= '9' && digits < 3) {
                    val = val * 10 + (*s - '0');
                    s++;
                    digits++;
                }
                if (digits == 0 || val < 1 || val > 366)
                    return (char *)0;
                /* tm_yday is 0-based */
                tm->tm_yday = val - 1;
                break;
            }
            case 'n':
            case 't':
                /* Match any whitespace */
                while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
                    s++;
                break;
            case '%':
                /* Literal percent */
                if (*s != '%')
                    return (char *)0;
                s++;
                break;
            default:
                /* Unknown specifier — treat as literal */
                if (*s != *format)
                    return (char *)0;
                s++;
                break;
            }
            format++;
        } else if (*format == ' ') {
            /* Whitespace in format matches zero or more whitespace in input */
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
                s++;
            format++;
        } else {
            /* Literal character must match */
            if (*s != *format)
                return (char *)0;
            s++;
            format++;
        }
    }

    return (char *)s;
}

void tzset(void)
{
    daylight = 0;
    timezone = 0;
    tzname[0] = (char *)"UTC";
    tzname[1] = (char *)"UTC";
}

struct tm *getdate(const char *string)
{
    static struct tm out;
    char *end = strptime(string, "%Y-%m-%d %H:%M:%S", &out);
    if (!end) {
        getdate_err = 7;
        return (struct tm *)0;
    }
    getdate_err = 0;
    return &out;
}

/* ------------------------------------------------------------------ */
/*  mktime                                                             */
/* ------------------------------------------------------------------ */

time_t mktime(struct tm *tm)
{
    int sec, min, hour, mday, mon, year;

    if (!tm)
        return (time_t)-1;

    /* copy and normalise */
    sec  = tm->tm_sec;
    min  = tm->tm_min;
    hour = tm->tm_hour;
    mday = tm->tm_mday;
    mon  = tm->tm_mon;
    year = tm->tm_year + 1900;

    /* normalise seconds → minutes */
    min += sec / 60;
    sec  = sec % 60;
    if (sec < 0) { sec += 60; min--; }

    /* normalise minutes → hours */
    hour += min / 60;
    min   = min % 60;
    if (min < 0) { min += 60; hour--; }

    /* normalise hours → days */
    mday += hour / 24;
    hour  = hour % 24;
    if (hour < 0) { hour += 24; mday--; }

    /* normalise months → years */
    if (mon < 0) {
        int adj = (-mon + 11) / 12;
        year -= adj;
        mon  += adj * 12;
    }
    year += mon / 12;
    mon   = mon % 12;

    /* normalise days (handle underflow and overflow) */
    while (mday < 1) {
        mon--;
        if (mon < 0) { mon = 11; year--; }
        mday += _mon_days[_is_leap(year)][mon];
    }
    while (mday > _mon_days[_is_leap(year)][mon]) {
        mday -= _mon_days[_is_leap(year)][mon];
        mon++;
        if (mon > 11) { mon = 0; year++; }
    }

    /* count days from epoch (1970-01-01) */
    long days = 0;
    if (year >= 1970) {
        for (int y = 1970; y < year; y++)
            days += _days_in_year(y);
    } else {
        for (int y = year; y < 1970; y++)
            days -= _days_in_year(y);
    }

    int leap = _is_leap(year);
    int yday = 0;
    for (int m = 0; m < mon; m++)
        yday += _mon_days[leap][m];
    yday += mday - 1;
    days += yday;

    /* fill in derived fields */
    tm->tm_year  = year - 1900;
    tm->tm_mon   = mon;
    tm->tm_mday  = mday;
    tm->tm_hour  = hour;
    tm->tm_min   = min;
    tm->tm_sec   = sec;
    tm->tm_yday  = yday;
    tm->tm_wday  = (int)((days + 4) % 7);      /* epoch was Thursday */
    if (tm->tm_wday < 0)
        tm->tm_wday += 7;
    tm->tm_isdst = 0;
    tm->tm_gmtoff = 0;
    tm->tm_zone = "UTC";

    return (time_t)(days * 86400 + hour * 3600 + min * 60 + sec);
}

/* ------------------------------------------------------------------ */
/*  strftime                                                           */
/* ------------------------------------------------------------------ */

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm)
{
    size_t pos = 0;

    if (!s || max == 0 || !fmt || !tm)
        return 0;

    while (*fmt && pos + 1 < max) {
        if (*fmt != '%') {
            s[pos++] = *fmt++;
            continue;
        }

        fmt++;  /* skip '%' */

        /* ENH: POSIX E and O locale modifiers -- in the C/POSIX locale these
         * produce the same output as the unmodified specifier, so just skip. */
        if (*fmt == 'E' || *fmt == 'O') fmt++;

        switch (*fmt) {
        case 'A':   /* ENH: full weekday name (e.g. "Sunday") */
            if (tm->tm_wday >= 0 && tm->tm_wday <= 6)
                _append(s, max, &pos, _wday_full[tm->tm_wday]);
            break;
        case 'a':   /* abbreviated weekday name */
            if (tm->tm_wday >= 0 && tm->tm_wday <= 6)
                _append(s, max, &pos, _wday_abbr[tm->tm_wday]);
            break;
        case 'B':   /* ENH: full month name (e.g. "January") */
            if (tm->tm_mon >= 0 && tm->tm_mon <= 11)
                _append(s, max, &pos, _mon_full[tm->tm_mon]);
            break;
        case 'b':   /* abbreviated month name */
        case 'h':   /* synonym for %b */
            if (tm->tm_mon >= 0 && tm->tm_mon <= 11)
                _append(s, max, &pos, _mon_abbr[tm->tm_mon]);
            break;
        case 'C':   /* ENH: century number (year / 100), zero-padded */
            _append_num(s, max, &pos, (tm->tm_year + 1900) / 100, 2);
            break;
        case 'c':   /* locale date and time: "Sun Jan  1 00:00:00 1970" */
            if (tm->tm_wday >= 0 && tm->tm_wday <= 6)
                _append(s, max, &pos, _wday_abbr[tm->tm_wday]);
            _append(s, max, &pos, " ");
            if (tm->tm_mon >= 0 && tm->tm_mon <= 11)
                _append(s, max, &pos, _mon_abbr[tm->tm_mon]);
            _append(s, max, &pos, " ");
            /* day of month, space-padded to 2 */
            if (tm->tm_mday < 10) {
                _append(s, max, &pos, " ");
                _append_num(s, max, &pos, tm->tm_mday, 1);
            } else {
                _append_num(s, max, &pos, tm->tm_mday, 2);
            }
            _append(s, max, &pos, " ");
            _append_num(s, max, &pos, tm->tm_hour, 2);
            _append(s, max, &pos, ":");
            _append_num(s, max, &pos, tm->tm_min, 2);
            _append(s, max, &pos, ":");
            _append_num(s, max, &pos, tm->tm_sec, 2);
            _append(s, max, &pos, " ");
            _append_num(s, max, &pos, tm->tm_year + 1900, 4);
            break;
        case 'D':   /* ENH: equivalent to %m/%d/%y (US date format) */
            _append_num(s, max, &pos, tm->tm_mon + 1, 2);
            _append(s, max, &pos, "/");
            _append_num(s, max, &pos, tm->tm_mday, 2);
            _append(s, max, &pos, "/");
            _append_num(s, max, &pos, (tm->tm_year + 1900) % 100, 2);
            break;
        case 'd':   /* day 01-31 */
            _append_num(s, max, &pos, tm->tm_mday, 2);
            break;
        case 'e':   /* ENH: day of month, space-padded (e.g. " 5") */
            if (tm->tm_mday < 10) {
                _append(s, max, &pos, " ");
                _append_num(s, max, &pos, tm->tm_mday, 1);
            } else {
                _append_num(s, max, &pos, tm->tm_mday, 2);
            }
            break;
        case 'F':   /* ENH: ISO 8601 date: %Y-%m-%d */
            _append_num(s, max, &pos, tm->tm_year + 1900, 4);
            _append(s, max, &pos, "-");
            _append_num(s, max, &pos, tm->tm_mon + 1, 2);
            _append(s, max, &pos, "-");
            _append_num(s, max, &pos, tm->tm_mday, 2);
            break;
        case 'G':   /* ENH: ISO week-based year (simplified: same as %Y) */
            _append_num(s, max, &pos, tm->tm_year + 1900, 4);
            break;
        case 'H':   /* hour 00-23 */
            _append_num(s, max, &pos, tm->tm_hour, 2);
            break;
        case 'I': { /* ENH: 12-hour clock hour, zero-padded 01-12 */
            int h12 = tm->tm_hour % 12;
            if (h12 == 0) h12 = 12;
            _append_num(s, max, &pos, h12, 2);
            break;
        }
        case 'j':   /* day of year 001-366 */
            _append_num(s, max, &pos, tm->tm_yday + 1, 3);
            break;
        case 'M':   /* minute 00-59 */
            _append_num(s, max, &pos, tm->tm_min, 2);
            break;
        case 'm':   /* month 01-12 */
            _append_num(s, max, &pos, tm->tm_mon + 1, 2);
            break;
        case 'n':   /* newline */
            s[pos++] = '\n';
            break;
        case 'p':   /* AM/PM */
            _append(s, max, &pos, tm->tm_hour < 12 ? "AM" : "PM");
            break;
        case 'R':   /* ENH: 24-hour HH:MM, equivalent to %H:%M */
            _append_num(s, max, &pos, tm->tm_hour, 2);
            _append(s, max, &pos, ":");
            _append_num(s, max, &pos, tm->tm_min, 2);
            break;
        case 'r': { /* ENH: 12-hour time, equivalent to %I:%M:%S %p */
            int h12 = tm->tm_hour % 12;
            if (h12 == 0) h12 = 12;
            _append_num(s, max, &pos, h12, 2);
            _append(s, max, &pos, ":");
            _append_num(s, max, &pos, tm->tm_min, 2);
            _append(s, max, &pos, ":");
            _append_num(s, max, &pos, tm->tm_sec, 2);
            _append(s, max, &pos, " ");
            _append(s, max, &pos, tm->tm_hour < 12 ? "AM" : "PM");
            break;
        }
        case 'S':   /* second 00-59 */
            _append_num(s, max, &pos, tm->tm_sec, 2);
            break;
        case 'T':   /* ENH: full time, equivalent to %H:%M:%S */
            _append_num(s, max, &pos, tm->tm_hour, 2);
            _append(s, max, &pos, ":");
            _append_num(s, max, &pos, tm->tm_min, 2);
            _append(s, max, &pos, ":");
            _append_num(s, max, &pos, tm->tm_sec, 2);
            break;
        case 't':   /* tab */
            s[pos++] = '\t';
            break;
        case 'U':   /* ENH: week number, Sunday as first day (00-53) */
            _append_num(s, max, &pos, (tm->tm_yday + 7 - tm->tm_wday) / 7, 2);
            break;
        case 'u':   /* ENH: day of week 1-7, Monday=1 */
            _append_num(s, max, &pos, tm->tm_wday == 0 ? 7 : tm->tm_wday, 1);
            break;
        case 'V': { /* ENH: ISO 8601 week number 01-53 (simplified) */
            int dow = (tm->tm_wday + 6) % 7;  /* Monday=0 */
            int wk = (tm->tm_yday + 7 - dow) / 7;
            /* ISO weeks are 01-53; week 0 belongs to previous year's last week */
            if (wk == 0) wk = 53;
            _append_num(s, max, &pos, wk, 2);
            break;
        }
        case 'W': { /* ENH: week number, Monday as first day (00-53) */
            int dow = (tm->tm_wday + 6) % 7;
            _append_num(s, max, &pos, (tm->tm_yday + 7 - dow) / 7, 2);
            break;
        }
        case 'w':   /* ENH: day of week 0-6, Sunday=0 */
            _append_num(s, max, &pos, tm->tm_wday, 1);
            break;
        case 'X':   /* ENH: locale time, equivalent to %H:%M:%S in C locale */
            _append_num(s, max, &pos, tm->tm_hour, 2);
            _append(s, max, &pos, ":");
            _append_num(s, max, &pos, tm->tm_min, 2);
            _append(s, max, &pos, ":");
            _append_num(s, max, &pos, tm->tm_sec, 2);
            break;
        case 'x':   /* ENH: locale date, equivalent to %m/%d/%y in C locale */
            _append_num(s, max, &pos, tm->tm_mon + 1, 2);
            _append(s, max, &pos, "/");
            _append_num(s, max, &pos, tm->tm_mday, 2);
            _append(s, max, &pos, "/");
            _append_num(s, max, &pos, (tm->tm_year + 1900) % 100, 2);
            break;
        case 'Y':   /* 4-digit year */
            _append_num(s, max, &pos, tm->tm_year + 1900, 4);
            break;
        case 'y':   /* ENH: 2-digit year (00-99) */
            _append_num(s, max, &pos, (tm->tm_year + 1900) % 100, 2);
            break;
        case 'Z':   /* timezone name */
            _append(s, max, &pos, "UTC");
            break;
        case 'z':   /* ENH: UTC offset -- hardcoded to +0000 (UTC only) */
            _append(s, max, &pos, "+0000");
            break;
        case '%':   /* literal % */
            s[pos++] = '%';
            break;
        case '\0':  /* trailing % at end of format string */
            goto done;
        default:    /* unknown specifier — output literally */
            s[pos++] = '%';
            if (pos + 1 < max)
                s[pos++] = *fmt;
            break;
        }
        fmt++;
    }

done:
    s[pos] = '\0';
    return pos;
}

size_t strftime_l(char *s, size_t max, const char *fmt,
                  const struct tm *tm, locale_t locale)
{
    (void)locale;
    return strftime(s, max, fmt, tm);
}

/*
 * nanosleep() — sleep for at least the duration specified by *req.
 * If interrupted, the remaining time is written to *rem (if non-NULL).
 */
int nanosleep(const struct timespec *req, struct timespec *rem) {
    long ret = syscall(SYS_nanosleep, req, rem);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/*
 * clock_gettime() — read a POSIX clock.
 * On SBUnix the kernel only handles CLOCK_REALTIME / CLOCK_MONOTONIC
 * via SYS_clock_gettime (113).  Returns 0 on success, -1 on error.
 */
int clock_gettime(int clk_id, struct timespec *tp) {
    long ret = syscall(SYS_clock_gettime, clk_id, tp);
    /* BF: Set errno from negative kernel return.  The clock_getres wrapper
     * below does this correctly; clock_gettime was missing it, causing
     * POSIX tests for invalid clock IDs to fail. */
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int clock_getres(clockid_t clockid, struct timespec *res) {
    long ret = syscall(SYS_clock_getres, clockid, res);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int clock_settime(clockid_t clockid, const struct timespec *tp) {
    long ret = syscall(SYS_clock_settime, clockid, tp);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int clock_getcpuclockid(pid_t pid, clockid_t *clock_id) {
    (void)pid;
    if (!clock_id) {
        errno = EINVAL;
        return -1;
    }
    *clock_id = CLOCK_PROCESS_CPUTIME_ID;
    return 0;
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

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv) {
        errno = EINVAL;
        return -1;
    }
    long ret = syscall(SYS_gettimeofday, tv, tz);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

int clock_nanosleep(int clockid, int flags,
                    const struct timespec *req, struct timespec *rem) {
    long ret = syscall(SYS_clock_nanosleep, clockid, flags, req, rem);
    if (ret < 0) { errno = (int)(-ret); return (int)(-ret); }
    return 0;
}

clock_t clock(void) {
    /* BF: Use RISC-V rdtime directly instead of clock_gettime syscall.
     * The previous implementation made a full ecall per call, causing
     * POSIX tests that call clock() in tight loops (e.g., clock_gettime/4-1
     * with 900,000 iterations) to time out under the 5-second alarm.
     * SBUnix exposes QEMU virt's 10MHz user rdtime ABI here; wall-clock
     * calls still route through kernel syscalls. */
    unsigned long ticks;
    asm volatile("rdtime %0" : "=r"(ticks));
    return (clock_t)(ticks / (10000000UL / CLOCKS_PER_SEC));
}

double difftime(time_t time1, time_t time0) {
  if (time1 >= time0) {
    return (double)(long)(time1 - time0);
  }
  return -(double)(long)(time0 - time1);
}

int timer_create(clockid_t clockid, struct sigevent *evp, timer_t *timerid) {
    long ret = syscall(SYS_timer_create, clockid, evp, timerid);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int timer_delete(timer_t timerid) {
    long ret = syscall(SYS_timer_delete, timerid);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int timer_getoverrun(timer_t timerid) {
    long ret = syscall(SYS_timer_getoverrun, timerid);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int timer_gettime(timer_t timerid, struct itimerspec *value) {
    long ret = syscall(SYS_timer_gettime, timerid, value);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int timer_settime(timer_t timerid, int flags,
                  const struct itimerspec *value,
                  struct itimerspec *ovalue) {
    long ret = syscall(SYS_timer_settime, timerid, flags, value, ovalue);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int timespec_get(struct timespec *ts, int base) {
    if (!ts || base != TIME_UTC)
        return 0;
    if (clock_gettime(CLOCK_REALTIME, ts) < 0)
        return 0;
    return base;
}

int getitimer(int which, struct itimerval *curr_value) {
    (void)which;
    if (!curr_value) {
        errno = EINVAL;
        return -1;
    }
    curr_value->it_interval.tv_sec = 0;
    curr_value->it_interval.tv_usec = 0;
    curr_value->it_value.tv_sec = 0;
    curr_value->it_value.tv_usec = 0;
    return 0;
}

int setitimer(int which, const struct itimerval *new_value,
              struct itimerval *old_value) {
    (void)which;
    (void)new_value;
    if (old_value) {
        old_value->it_interval.tv_sec = 0;
        old_value->it_interval.tv_usec = 0;
        old_value->it_value.tv_sec = 0;
        old_value->it_value.tv_usec = 0;
    }
    return 0;
}
