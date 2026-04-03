/*
 * locale.c -- POSIX "C" locale implementation for SBUnix libc.
 *
 * Supports the "C" and "POSIX" locales (which are identical per POSIX).
 * The empty string "" selects the default "C" locale.  Any other locale
 * name is rejected with a NULL return.
 *
 * setlocale() tracks the active locale per category and returns the
 * current locale name.  localeconv() returns a pointer to a static
 * struct lconv populated with the standard POSIX/C locale values.
 */

#include <locale.h>
#include <string.h>
#include <limits.h>

static int c_locale_obj;
static locale_t current_thread_locale = (locale_t)&c_locale_obj;

/* Number of locale categories (LC_ALL through LC_MESSAGES) */
#define NUM_CATEGORIES 7

/*
 * Per-category locale name.  In the "C" locale all categories share the
 * same name, so we just store a pointer to one of the two accepted
 * string literals.
 */
static const char *current_locale[NUM_CATEGORIES] = {
    "C", /* LC_ALL      */
    "C", /* LC_COLLATE  */
    "C", /* LC_CTYPE    */
    "C", /* LC_MONETARY */
    "C", /* LC_NUMERIC  */
    "C", /* LC_TIME     */
    "C", /* LC_MESSAGES */
};

/*
 * Static lconv structure pre-filled with the POSIX "C" locale defaults.
 * POSIX specifies:
 *   - decimal_point  = "."
 *   - All other string fields = ""
 *   - negative_sign  = ""  (POSIX C locale: empty; we use "-" as a
 *     practical default for printf-family consumers that inspect lconv)
 *   - All char fields = CHAR_MAX  (meaning "unspecified")
 */
static struct lconv c_lconv = {
    .decimal_point      = ".",
    .thousands_sep      = "",
    .grouping           = "",
    .int_curr_symbol    = "",
    .currency_symbol    = "",
    .mon_decimal_point  = "",
    .mon_thousands_sep  = "",
    .mon_grouping       = "",
    .positive_sign      = "",
    .negative_sign      = "-",
    .int_frac_digits    = (char)CHAR_MAX,
    .frac_digits        = (char)CHAR_MAX,
    .p_cs_precedes      = (char)CHAR_MAX,
    .p_sep_by_space     = (char)CHAR_MAX,
    .n_cs_precedes      = (char)CHAR_MAX,
    .n_sep_by_space     = (char)CHAR_MAX,
    .p_sign_posn        = (char)CHAR_MAX,
    .n_sign_posn        = (char)CHAR_MAX,
    .int_p_cs_precedes  = (char)CHAR_MAX,
    .int_p_sep_by_space = (char)CHAR_MAX,
    .int_n_cs_precedes  = (char)CHAR_MAX,
    .int_n_sep_by_space = (char)CHAR_MAX,
    .int_p_sign_posn    = (char)CHAR_MAX,
    .int_n_sign_posn    = (char)CHAR_MAX,
};

/*
 * Helper: return non-zero if `name` is a supported locale ("C", "POSIX",
 * or "" which maps to the default "C" locale).
 */
static int locale_is_valid(const char *name)
{
    if (name[0] == '\0')
        return 1;
    if (strcmp(name, "C") == 0)
        return 1;
    if (strcmp(name, "POSIX") == 0)
        return 1;
    return 0;
}

/*
 * setlocale -- set or query the program's current locale.
 *
 * If `locale` is NULL the current locale name for `category` is returned
 * without changing anything.  Otherwise the locale for `category` (or
 * all categories when category == LC_ALL) is set.
 *
 * Returns the locale name on success, or NULL if the requested locale
 * is not supported.
 */
char *setlocale(int category, const char *locale)
{
    /* Validate category */
    if (category < LC_ALL || category > LC_MESSAGES)
        return (char *)0;

    /* Query mode: just return the current locale name */
    if (locale == (const char *)0)
        return (char *)current_locale[category];

    /* Reject unsupported locales */
    if (!locale_is_valid(locale))
        return (char *)0;

    /*
     * Set the locale.  "", "C", and "POSIX" all resolve to the same
     * canonical name "C".
     */
    if (category == LC_ALL) {
        int i;
        for (i = 0; i < NUM_CATEGORIES; i++)
            current_locale[i] = "C";
    } else {
        current_locale[category] = "C";
    }

    return (char *)current_locale[category];
}

/*
 * localeconv -- return a pointer to the lconv structure for the current
 * locale.  Since we only support the "C" locale, this always returns
 * the same static structure.
 */
struct lconv *localeconv(void)
{
    return &c_lconv;
}

locale_t duplocale(locale_t locobj)
{
    if (!locobj || locobj == LC_GLOBAL_LOCALE)
        return (locale_t)&c_locale_obj;
    return locobj;
}

void freelocale(locale_t locobj)
{
    (void)locobj;
}

locale_t newlocale(int category_mask, const char *locale, locale_t base)
{
    (void)category_mask;
    (void)base;
    if (!locale)
        return (locale_t)0;
    if (!locale_is_valid(locale))
        return (locale_t)0;
    return (locale_t)&c_locale_obj;
}

locale_t uselocale(locale_t newloc)
{
    locale_t old = current_thread_locale;
    if (!newloc)
        return old;
    if (newloc == LC_GLOBAL_LOCALE)
        return old;
    current_thread_locale = newloc;
    return old;
}

const char *getlocalename_l(int category, locale_t locale)
{
    (void)locale;
    if (category < LC_ALL || category > LC_MESSAGES)
        return (const char *)0;
    return "C";
}
