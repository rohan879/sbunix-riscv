#include <langinfo.h>

static char *const days[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

static char *const abdays[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static char *const months[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static char *const abmonths[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static char *_nl_langinfo_c(nl_item item)
{
    if (item >= DAY_1 && item <= DAY_7)
        return days[item - DAY_1];
    if (item >= ABDAY_1 && item <= ABDAY_7)
        return abdays[item - ABDAY_1];
    if (item >= MON_1 && item <= MON_12)
        return months[item - MON_1];
    if (item >= ABMON_1 && item <= ABMON_12)
        return abmonths[item - ABMON_1];
    switch (item) {
    case CODESET: return "UTF-8";
    case D_T_FMT: return "%a %b %e %H:%M:%S %Y";
    case D_FMT: return "%m/%d/%y";
    case T_FMT: return "%H:%M:%S";
    case T_FMT_AMPM: return "%I:%M:%S %p";
    case AM_STR: return "AM";
    case PM_STR: return "PM";
    case ERA: return "";
    case ERA_D_FMT: return "";
    case ERA_D_T_FMT: return "";
    case ERA_T_FMT: return "";
    case ALT_DIGITS: return "";
    case RADIXCHAR: return ".";
    case THOUSEP: return "";
    case YESEXPR: return "^[yY]";
    case NOEXPR: return "^[nN]";
    case CRNCYSTR: return "-";
    case __ALTMON_1: return months[0];
    case __ALTMON_2: return months[1];
    case __ALTMON_3: return months[2];
    case __ALTMON_4: return months[3];
    case __ALTMON_5: return months[4];
    case __ALTMON_6: return months[5];
    case __ALTMON_7: return months[6];
    case __ALTMON_8: return months[7];
    case __ALTMON_9: return months[8];
    case __ALTMON_10: return months[9];
    case __ALTMON_11: return months[10];
    case __ALTMON_12: return months[11];
    default: return "";
    }
}

char *nl_langinfo(nl_item item)
{
    return _nl_langinfo_c(item);
}

char *nl_langinfo_l(nl_item item, locale_t locale)
{
    (void)locale;
    return _nl_langinfo_c(item);
}
