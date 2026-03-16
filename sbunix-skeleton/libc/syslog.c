/*
 * syslog.c -- syslog implementation for SBUnix libc.
 *
 * Since SBUnix has no syslog daemon, log messages are written directly
 * to stderr (fd 2), which maps to the kernel console via the UART.
 * This provides real, visible logging output rather than silently
 * discarding messages.
 *
 * Output format:  ident[pid]: message\n     (if LOG_PID is set)
 *                 ident: message\n          (otherwise)
 */

#include <syslog.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

/* Internal state */
static const char *log_ident    = (const char *)0;
static int         log_option   = 0;
static int         log_facility = LOG_USER;
static int         log_mask     = 0xFF;  /* all priorities enabled */

/* Priority-level name table, indexed by LOG_EMERG..LOG_DEBUG */
static const char *const priority_names[] = {
    "emerg",
    "alert",
    "crit",
    "err",
    "warning",
    "notice",
    "info",
    "debug",
};

/*
 * openlog -- set the identity string, option flags, and default
 * facility for subsequent syslog() calls.
 *
 * `ident` is stored by pointer (POSIX permits this); the caller must
 * keep it valid until closelog() or a new openlog() call.
 *
 * If LOG_NDELAY is specified we would normally open the log destination
 * immediately; since we write to fd 2 (always open) this is a no-op
 * but we honour the flag for correctness.
 */
void openlog(const char *ident, int option, int facility)
{
    log_ident    = ident;
    log_option   = option;
    log_facility = facility & LOG_FACMASK;

    /* LOG_NDELAY: nothing extra needed -- fd 2 is always available */
    (void)0;
}

/*
 * syslog -- format and write a log message to the console.
 *
 * The `priority` argument encodes both a facility (optional, ORed in)
 * and a severity level.  If the caller did not include a facility the
 * default facility from openlog() is used.
 *
 * Messages whose priority level is not enabled in the current log mask
 * (set by setlogmask) are silently dropped.
 */
void syslog(int priority, const char *format, ...)
{
    int pri;
    const char *ident;
    char msgbuf[512];
    char linebuf[768];
    int  pos;
    va_list ap;

    /* Extract priority level from the combined priority argument.
     * The facility portion is not used in this embedded implementation
     * (no daemon to route by facility); only the severity matters. */
    pri = LOG_PRI(priority);

    /* Check log mask -- drop messages not enabled */
    if (!(log_mask & LOG_MASK(pri)))
        return;

    /* Clamp priority for the name table lookup */
    if (pri < 0)
        pri = 0;
    if (pri > LOG_DEBUG)
        pri = LOG_DEBUG;

    /* Format the caller's message into msgbuf */
    va_start(ap, format);
    vsnprintf(msgbuf, sizeof(msgbuf), format, ap);
    va_end(ap);

    /* Choose ident string */
    ident = log_ident;
    if (ident == (const char *)0 || ident[0] == '\0')
        ident = "syslog";

    /* Build the output line: <priority> ident[pid]: message */
    pos = snprintf(linebuf, sizeof(linebuf), "<%s> %s",
                   priority_names[pri], ident);

    /* Append PID if LOG_PID option is set */
    if (log_option & LOG_PID) {
        pos += snprintf(linebuf + pos, sizeof(linebuf) - (unsigned long)pos,
                        "[%d]", (int)getpid());
    }

    /* Append the message */
    pos += snprintf(linebuf + pos, sizeof(linebuf) - (unsigned long)pos,
                    ": %s\n", msgbuf);

    /* Write to stderr (kernel console) */
    write(STDERR_FILENO, linebuf, (size_t)pos);

    /* LOG_PERROR: also write to stderr -- already our destination, so
     * this is inherently satisfied.  LOG_CONS likewise. */
}

/*
 * closelog -- reset syslog state.
 *
 * After closelog(), syslog() will still work but will use defaults
 * (ident = "syslog", facility = LOG_USER).
 */
void closelog(void)
{
    log_ident    = (const char *)0;
    log_option   = 0;
    log_facility = LOG_USER;
    /* log_mask is intentionally preserved across closelog per POSIX */
}

/*
 * setlogmask -- set the log priority mask.
 *
 * Only messages whose priority level has its corresponding bit set in
 * `mask` will be logged.  Use LOG_MASK(pri) or LOG_UPTO(pri) to build
 * the mask value.
 *
 * If `mask` is 0, the current mask is returned without modification.
 *
 * Returns the previous mask value.
 */
int setlogmask(int mask)
{
    int old = log_mask;
    if (mask != 0)
        log_mask = mask;
    return old;
}
