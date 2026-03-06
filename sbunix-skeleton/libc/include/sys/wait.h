#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/types.h>

/* waitpid() / wait() options */
#define WNOHANG    1  /* do not block if no child has exited */
#define WUNTRACED  2  /* also return for stopped children */
#define WCONTINUED 8  /* also return for continued children */

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

/* Construct a "normal exit" status value (for testing) */
#define W_EXITCODE(ret, sig) (((ret) << 8) | (sig))

/* POSIX wait functions */
pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);

#endif /* _SYS_WAIT_H */
