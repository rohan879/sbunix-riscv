#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/types.h>
#include <signal.h>

/* waitpid() / wait() options */
#define WNOHANG    1  /* do not block if no child has exited */
#define WUNTRACED  2  /* also return for stopped children */
/* BF: waitid(WSTOPPED) uses the same stopped-child bit as WUNTRACED.
 * Bit 16 is WTRAPPED, not the normal stopped-child request. */
#define WSTOPPED   2
#define WEXITED    4
#define WCONTINUED 8  /* also return for continued children */
#define WTRAPPED   16
#define WNOWAIT    0x01000000

/* Wait-status inspection macros */

/* True if child exited normally (via exit() / return from main) */
#define WIFEXITED(s)    (((s) & 0x7f) == 0)

/* Extract exit code when WIFEXITED is true */
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)

/* True if child was killed by a signal */
#define WIFSIGNALED(s)  (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)

/* Extract the signal number when WIFSIGNALED is true */
#define WTERMSIG(s)     ((s) & 0x7f)

/* True if child was stopped by a signal (with WUNTRACED) */
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)

/* Extract the stop signal when WIFSTOPPED is true */
#define WSTOPSIG(s)     (((s) >> 8) & 0xff)

/* True if stopped child was continued (with WCONTINUED) */
#define WIFCONTINUED(s) ((s) == 0xffff)
#define WCOREDUMP(s)    0

/* Construct a "normal exit" status value (for testing) */
#define W_EXITCODE(ret, sig) (((ret) << 8) | (sig))

typedef enum {
  P_ALL = 0,
  P_PID = 1,
  P_PGID = 2,
} idtype_t;

/* POSIX wait functions */
pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);
int waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options);

#endif /* _SYS_WAIT_H */
