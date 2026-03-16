/*
 * libxtest.c — comprehensive tests for dlfcn, regex, locale, and syslog.
 *
 * All tests exercise real functionality on the running OS.
 * No mocks, no trivial checks.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <regex.h>
#include <locale.h>
#include <syslog.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { \
        printf("[PASS] libx: %s\n", label); \
        g_pass++; \
    } else { \
        printf("[FAIL] libx: %s\n", label); \
        g_fail++; \
    } \
} while (0)

/* =================================================================
 * 1. dlfcn — dynamic symbol lookup via built-in table
 * ================================================================= */
static void test_dlfcn(void) {
    /* dlopen(NULL) returns self handle */
    void *h = dlopen(NULL, RTLD_NOW);
    CHECK("dlopen(NULL) returns handle", h != NULL);

    /* dlerror is NULL after success */
    char *err = dlerror();
    CHECK("dlerror NULL after success", err == NULL);

    /* dlsym finds printf */
    void *sym = dlsym(h, "printf");
    CHECK("dlsym finds printf", sym != NULL);
    CHECK("dlsym printf is correct addr", sym == (void *)printf);

    /* dlsym finds malloc */
    sym = dlsym(h, "malloc");
    CHECK("dlsym finds malloc", sym != NULL);
    CHECK("dlsym malloc is correct addr", sym == (void *)malloc);

    /* dlsym finds strlen */
    sym = dlsym(h, "strlen");
    CHECK("dlsym finds strlen", sym != NULL);

    /* dlsym finds fork */
    sym = dlsym(h, "fork");
    CHECK("dlsym finds fork", sym != NULL);

    /* dlsym finds stat */
    sym = dlsym(h, "stat");
    CHECK("dlsym finds stat", sym != NULL);

    /* dlsym finds mmap */
    sym = dlsym(h, "mmap");
    CHECK("dlsym finds mmap", sym != NULL);

    /* dlsym returns NULL for unknown symbol */
    sym = dlsym(h, "nonexistent_function_xyz");
    CHECK("dlsym returns NULL for unknown", sym == NULL);

    /* dlerror set after failure */
    err = dlerror();
    CHECK("dlerror set after unknown sym", err != NULL);
    CHECK("dlerror mentions symbol name", err && strstr(err, "nonexistent_function_xyz") != NULL);

    /* dlerror cleared after retrieval */
    err = dlerror();
    CHECK("dlerror cleared after read", err == NULL);

    /* dlsym with RTLD_DEFAULT also works */
    sym = dlsym(RTLD_DEFAULT, "write");
    CHECK("dlsym RTLD_DEFAULT finds write", sym != NULL);

    /* Actually call a function found via dlsym */
    typedef size_t (*strlen_fn)(const char *);
    strlen_fn my_strlen = (strlen_fn)dlsym(h, "strlen");
    CHECK("dlsym strlen callable", my_strlen != NULL);
    if (my_strlen) {
        size_t len = my_strlen("hello");
        CHECK("dlsym strlen('hello')=5", len == 5);
    }

    /* dlclose succeeds */
    int r = dlclose(h);
    CHECK("dlclose returns 0", r == 0);

    /* dlopen with filename fails (no .so support) */
    void *h2 = dlopen("libfoo.so", RTLD_NOW);
    CHECK("dlopen(libfoo.so) returns NULL", h2 == NULL);
    err = dlerror();
    CHECK("dlerror set for .so", err != NULL);
}

/* =================================================================
 * 2. regex — pattern matching
 * ================================================================= */
static void test_regex(void) {
    regex_t re;
    regmatch_t pmatch[4];
    int r;

    /* Basic literal match */
    r = regcomp(&re, "hello", 0);
    CHECK("regex: compile literal", r == 0);
    r = regexec(&re, "say hello world", 0, NULL, 0);
    CHECK("regex: literal match", r == 0);
    r = regexec(&re, "say goodbye", 0, NULL, 0);
    CHECK("regex: literal no match", r == REG_NOMATCH);
    regfree(&re);

    /* Anchored pattern: ^hello$ */
    r = regcomp(&re, "^hello$", REG_EXTENDED);
    CHECK("regex: compile ^hello$", r == 0);
    r = regexec(&re, "hello", 0, NULL, 0);
    CHECK("regex: ^hello$ matches 'hello'", r == 0);
    r = regexec(&re, "hello world", 0, NULL, 0);
    CHECK("regex: ^hello$ rejects 'hello world'", r == REG_NOMATCH);
    r = regexec(&re, "say hello", 0, NULL, 0);
    CHECK("regex: ^hello$ rejects 'say hello'", r == REG_NOMATCH);
    regfree(&re);

    /* Character class [a-z]+ */
    r = regcomp(&re, "^[a-z]+$", REG_EXTENDED);
    CHECK("regex: compile [a-z]+", r == 0);
    r = regexec(&re, "hello", 0, NULL, 0);
    CHECK("regex: [a-z]+ matches 'hello'", r == 0);
    r = regexec(&re, "Hello", 0, NULL, 0);
    CHECK("regex: [a-z]+ rejects 'Hello'", r == REG_NOMATCH);
    r = regexec(&re, "hello123", 0, NULL, 0);
    CHECK("regex: [a-z]+ rejects 'hello123'", r == REG_NOMATCH);
    r = regexec(&re, "", 0, NULL, 0);
    CHECK("regex: [a-z]+ rejects empty", r == REG_NOMATCH);
    regfree(&re);

    /* Dot matches any */
    r = regcomp(&re, "^a.c$", REG_EXTENDED);
    CHECK("regex: compile a.c", r == 0);
    r = regexec(&re, "abc", 0, NULL, 0);
    CHECK("regex: a.c matches 'abc'", r == 0);
    r = regexec(&re, "axc", 0, NULL, 0);
    CHECK("regex: a.c matches 'axc'", r == 0);
    r = regexec(&re, "ac", 0, NULL, 0);
    CHECK("regex: a.c rejects 'ac'", r == REG_NOMATCH);
    regfree(&re);

    /* Alternation */
    r = regcomp(&re, "^(cat|dog)$", REG_EXTENDED);
    CHECK("regex: compile cat|dog", r == 0);
    r = regexec(&re, "cat", 0, NULL, 0);
    CHECK("regex: cat|dog matches 'cat'", r == 0);
    r = regexec(&re, "dog", 0, NULL, 0);
    CHECK("regex: cat|dog matches 'dog'", r == 0);
    r = regexec(&re, "bird", 0, NULL, 0);
    CHECK("regex: cat|dog rejects 'bird'", r == REG_NOMATCH);
    regfree(&re);

    /* Quantifiers: ? and * */
    r = regcomp(&re, "^ab?c$", REG_EXTENDED);
    CHECK("regex: compile ab?c", r == 0);
    r = regexec(&re, "ac", 0, NULL, 0);
    CHECK("regex: ab?c matches 'ac'", r == 0);
    r = regexec(&re, "abc", 0, NULL, 0);
    CHECK("regex: ab?c matches 'abc'", r == 0);
    r = regexec(&re, "abbc", 0, NULL, 0);
    CHECK("regex: ab?c rejects 'abbc'", r == REG_NOMATCH);
    regfree(&re);

    r = regcomp(&re, "^ab*c$", REG_EXTENDED);
    CHECK("regex: compile ab*c", r == 0);
    r = regexec(&re, "ac", 0, NULL, 0);
    CHECK("regex: ab*c matches 'ac'", r == 0);
    r = regexec(&re, "abbc", 0, NULL, 0);
    CHECK("regex: ab*c matches 'abbc'", r == 0);
    r = regexec(&re, "abbbbbc", 0, NULL, 0);
    CHECK("regex: ab*c matches 'abbbbbc'", r == 0);
    regfree(&re);

    /* + quantifier */
    r = regcomp(&re, "^ab+c$", REG_EXTENDED);
    CHECK("regex: compile ab+c", r == 0);
    r = regexec(&re, "abc", 0, NULL, 0);
    CHECK("regex: ab+c matches 'abc'", r == 0);
    r = regexec(&re, "abbc", 0, NULL, 0);
    CHECK("regex: ab+c matches 'abbc'", r == 0);
    r = regexec(&re, "ac", 0, NULL, 0);
    CHECK("regex: ab+c rejects 'ac'", r == REG_NOMATCH);
    regfree(&re);

    /* REG_ICASE */
    r = regcomp(&re, "^hello$", REG_EXTENDED | REG_ICASE);
    CHECK("regex: compile icase", r == 0);
    r = regexec(&re, "hello", 0, NULL, 0);
    CHECK("regex: icase matches 'hello'", r == 0);
    r = regexec(&re, "HELLO", 0, NULL, 0);
    CHECK("regex: icase matches 'HELLO'", r == 0);
    r = regexec(&re, "HeLLo", 0, NULL, 0);
    CHECK("regex: icase matches 'HeLLo'", r == 0);
    regfree(&re);

    /* Negated character class [^0-9] */
    r = regcomp(&re, "^[^0-9]+$", REG_EXTENDED);
    CHECK("regex: compile [^0-9]+", r == 0);
    r = regexec(&re, "hello", 0, NULL, 0);
    CHECK("regex: [^0-9]+ matches 'hello'", r == 0);
    r = regexec(&re, "hello123", 0, NULL, 0);
    CHECK("regex: [^0-9]+ rejects 'hello123'", r == REG_NOMATCH);
    regfree(&re);

    /* Subexpression capture */
    r = regcomp(&re, "^([a-z]+)=([0-9]+)$", REG_EXTENDED);
    CHECK("regex: compile group capture", r == 0);
    CHECK("regex: nsub == 2", re.re_nsub == 2);
    r = regexec(&re, "key=42", 3, pmatch, 0);
    CHECK("regex: capture matches", r == 0);
    if (r == 0) {
        CHECK("regex: full match 0-6", pmatch[0].rm_so == 0 && pmatch[0].rm_eo == 6);
        CHECK("regex: group1 'key' 0-3", pmatch[1].rm_so == 0 && pmatch[1].rm_eo == 3);
        CHECK("regex: group2 '42' 4-6", pmatch[2].rm_so == 4 && pmatch[2].rm_eo == 6);
    }
    regfree(&re);

    /* Unanchored match — finds substring */
    r = regcomp(&re, "[0-9]+", REG_EXTENDED);
    CHECK("regex: compile unanchored [0-9]+", r == 0);
    r = regexec(&re, "abc 123 def", 1, pmatch, 0);
    CHECK("regex: unanchored finds digits", r == 0);
    if (r == 0) {
        CHECK("regex: match at offset 4-7", pmatch[0].rm_so == 4 && pmatch[0].rm_eo == 7);
    }
    regfree(&re);

    /* regerror produces message */
    char errbuf[64];
    size_t len = regerror(REG_NOMATCH, NULL, errbuf, sizeof(errbuf));
    CHECK("regerror: returns length > 0", len > 0);
    CHECK("regerror: non-empty string", errbuf[0] != '\0');

    /* Bad pattern */
    r = regcomp(&re, "[unterminated", REG_EXTENDED);
    CHECK("regex: bad pattern returns error", r != 0);

    /* Empty pattern */
    r = regcomp(&re, "", REG_EXTENDED);
    CHECK("regex: empty pattern compiles", r == 0);
    r = regexec(&re, "anything", 0, NULL, 0);
    CHECK("regex: empty matches anything", r == 0);
    regfree(&re);
}

/* =================================================================
 * 3. locale — C/POSIX locale
 * ================================================================= */
static void test_locale(void) {
    /* setlocale with NULL returns current locale */
    char *loc = setlocale(LC_ALL, NULL);
    CHECK("locale: initial query non-NULL", loc != NULL);
    CHECK("locale: initial is 'C'", loc && strcmp(loc, "C") == 0);

    /* Set to "C" explicitly */
    loc = setlocale(LC_ALL, "C");
    CHECK("locale: set 'C' succeeds", loc != NULL);
    CHECK("locale: set 'C' returns 'C'", loc && strcmp(loc, "C") == 0);

    /* Set to "POSIX" */
    loc = setlocale(LC_ALL, "POSIX");
    CHECK("locale: set 'POSIX' succeeds", loc != NULL);

    /* Set to "" (use default = "C") */
    loc = setlocale(LC_ALL, "");
    CHECK("locale: set '' succeeds", loc != NULL);

    /* Unsupported locale returns NULL */
    loc = setlocale(LC_ALL, "en_US.UTF-8");
    CHECK("locale: unsupported returns NULL", loc == NULL);

    /* Current is still "C" after failed set */
    loc = setlocale(LC_ALL, NULL);
    CHECK("locale: still 'C' after fail", loc && strcmp(loc, "C") == 0);

    /* Per-category setting */
    loc = setlocale(LC_NUMERIC, "C");
    CHECK("locale: LC_NUMERIC set", loc != NULL);

    loc = setlocale(LC_TIME, NULL);
    CHECK("locale: LC_TIME query", loc != NULL);

    /* localeconv returns valid struct */
    struct lconv *lc = localeconv();
    CHECK("localeconv: non-NULL", lc != NULL);
    if (lc) {
        CHECK("localeconv: decimal_point='.'", strcmp(lc->decimal_point, ".") == 0);
        CHECK("localeconv: thousands_sep=''", strcmp(lc->thousands_sep, "") == 0);
        CHECK("localeconv: grouping=''", strcmp(lc->grouping, "") == 0);
        CHECK("localeconv: currency_symbol=''", strcmp(lc->currency_symbol, "") == 0);
        CHECK("localeconv: positive_sign=''", strcmp(lc->positive_sign, "") == 0);
        CHECK("localeconv: int_frac_digits=127", lc->int_frac_digits == 127);
        CHECK("localeconv: frac_digits=127", lc->frac_digits == 127);
    }
}

/* =================================================================
 * 4. syslog — log to console
 * ================================================================= */
static void test_syslog(void) {
    /*
     * Test syslog by redirecting fd 2 (stderr) to a file,
     * then reading back to verify messages were written and filtered.
     */

    /* Save original stderr */
    int saved_stderr = dup(2);

    /* Redirect stderr to a temp file */
    int logfd = open("/mnt/syslog_test.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(logfd, 2);
    close(logfd);

    /* Open syslog with ident and PID */
    openlog("mytest", LOG_PID, LOG_USER);

    /* Set mask to WARNING and above */
    setlogmask(LOG_UPTO(LOG_WARNING));

    /* Write a WARNING message — should appear */
    syslog(LOG_WARNING, "visible_warning_%d", 99);

    /* Write an ERR message — should appear (ERR < WARNING) */
    syslog(LOG_ERR, "visible_error");

    /* Write a DEBUG message — should be FILTERED (DEBUG > WARNING) */
    syslog(LOG_DEBUG, "hidden_debug");

    /* Write an INFO message — should be FILTERED */
    syslog(LOG_INFO, "hidden_info");

    /* Verify setlogmask returns previous mask */
    int prev = setlogmask(LOG_UPTO(LOG_DEBUG));
    CHECK("syslog: setlogmask returns prev mask", prev == LOG_UPTO(LOG_WARNING));

    /* Now write INFO — should appear (mask includes all) */
    syslog(LOG_INFO, "now_visible_info");

    /* closelog and reopen with different ident */
    closelog();
    openlog("other", 0, LOG_DAEMON);
    syslog(LOG_ERR, "other_ident_msg");
    closelog();

    /* Restore stderr */
    dup2(saved_stderr, 2);
    close(saved_stderr);

    /* Read back the log file and verify contents */
    int rfd = open("/mnt/syslog_test.log", O_RDONLY);
    CHECK("syslog: log file exists", rfd >= 0);
    char buf[1024] = {0};
    int n = 0;
    if (rfd >= 0) {
        n = read(rfd, buf, sizeof(buf) - 1);
        close(rfd);
    }
    CHECK("syslog: log file has content", n > 0);

    /* Verify WARNING message was written with ident */
    CHECK("syslog: contains 'mytest'", strstr(buf, "mytest") != NULL);
    CHECK("syslog: contains visible_warning_99", strstr(buf, "visible_warning_99") != NULL);
    CHECK("syslog: contains visible_error", strstr(buf, "visible_error") != NULL);

    /* Verify filtered messages are NOT present */
    CHECK("syslog: hidden_debug filtered out", strstr(buf, "hidden_debug") == NULL);
    CHECK("syslog: hidden_info filtered out", strstr(buf, "hidden_info") == NULL);

    /* Verify message appeared after mask change */
    CHECK("syslog: now_visible_info present", strstr(buf, "now_visible_info") != NULL);

    /* Verify ident changed after closelog+openlog */
    CHECK("syslog: other ident present", strstr(buf, "other") != NULL);
    CHECK("syslog: other_ident_msg present", strstr(buf, "other_ident_msg") != NULL);

    /* Verify LOG_PRI and LOG_FAC macros */
    int pri = LOG_MAKEPRI(LOG_USER, LOG_ERR);
    CHECK("syslog: LOG_PRI extracts priority", LOG_PRI(pri) == LOG_ERR);
    CHECK("syslog: LOG_FAC extracts facility", LOG_FAC(pri) == LOG_FAC(LOG_USER));

    /* LOG_MASK and LOG_UPTO arithmetic */
    CHECK("syslog: LOG_MASK(LOG_ERR)=8", LOG_MASK(LOG_ERR) == (1 << LOG_ERR));
    CHECK("syslog: LOG_UPTO(LOG_WARNING)=31", LOG_UPTO(LOG_WARNING) == ((1 << (LOG_WARNING + 1)) - 1));

    unlink("/mnt/syslog_test.log");
}

/* =================================================================
 * main
 * ================================================================= */
int main(void) {
    printf("=== libxtest: dlfcn/regex/locale/syslog tests ===\n");

    test_dlfcn();
    test_regex();
    test_locale();
    test_syslog();

    printf("=== libxtest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
