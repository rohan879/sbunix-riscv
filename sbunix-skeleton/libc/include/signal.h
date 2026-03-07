#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <sys/types.h>

/* Signal numbers (Linux RISC-V ABI) */
#define SIGHUP    1   /* hangup */
#define SIGINT    2   /* interrupt (Ctrl-C) */
#define SIGQUIT   3   /* quit */
#define SIGILL    4   /* illegal instruction */
#define SIGTRAP   5   /* trace/breakpoint trap */
#define SIGABRT   6   /* abort */
#define SIGBUS    7   /* bus error */
#define SIGFPE    8   /* floating-point exception */
#define SIGKILL   9   /* kill (cannot be caught) */
#define SIGUSR1   10  /* user-defined signal 1 */
#define SIGSEGV   11  /* segmentation fault */
#define SIGUSR2   12  /* user-defined signal 2 */
#define SIGPIPE   13  /* broken pipe */
#define SIGALRM   14  /* alarm clock */
#define SIGTERM   15  /* termination */
#define SIGSTKFLT 16  /* stack fault */
#define SIGCHLD   17  /* child stopped or exited */
#define SIGCONT   18  /* continue */
#define SIGSTOP   19  /* stop (cannot be caught) */
#define SIGTSTP   20  /* terminal stop */
#define SIGTTIN   21  /* background read from tty */
#define SIGTTOU   22  /* background write to tty */
#define SIGURG    23  /* urgent condition on socket */
#define SIGXCPU   24  /* CPU time limit exceeded */
#define SIGXFSZ   25  /* file size limit exceeded */
#define SIGVTALRM 26  /* virtual alarm clock */
#define SIGPROF   27  /* profiling alarm clock */
#define SIGWINCH  28  /* window size change */
#define SIGIO     29  /* I/O possible */
#define SIGPWR    30  /* power failure */
#define SIGSYS    31  /* bad system call */
#define NSIG      32  /* number of signals */

/* Special SIG_* values */
#define SIG_DFL  ((void (*)(int))0)   /* default action */
#define SIG_IGN  ((void (*)(int))1)   /* ignore signal */
#define SIG_ERR  ((void (*)(int))-1)  /* error return from signal() */

/* sigset_t — bitmask of signals */
typedef unsigned long sigset_t;

/* sig_atomic_t — integer type that can be accessed atomically */
typedef volatile int sig_atomic_t;

/* sighandler_t — convenience typedef for signal handler functions */
typedef void (*sighandler_t)(int);

/* sigaction flags */
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_RESTORER  0x04000000
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

/* siginfo_t — extended signal information (minimal subset) */
typedef struct {
    int     si_signo;   /* signal number */
    int     si_errno;   /* errno value */
    int     si_code;    /* signal code */
    pid_t   si_pid;     /* sending process PID */
    uid_t   si_uid;     /* sending process real UID */
    int     si_status;  /* exit value or signal */
    void   *si_addr;    /* faulting memory address */
} siginfo_t;

/* struct sigaction */
struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;
    int      sa_flags;
    void   (*sa_restorer)(void);
};

/* signal() — install a signal handler; returns previous handler */
void (*signal(int signum, void (*handler)(int)))(int);

/* sigaction() — examine/change signal action */
int sigaction(int signum, const struct sigaction *act,
              struct sigaction *oldact);

/* kill() — send signal to process */
int kill(pid_t pid, int sig);

/* raise() — send signal to self */
int raise(int sig);

/* Signal set manipulation */
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signum);
int sigdelset(sigset_t *set, int signum);
int sigismember(const sigset_t *set, int signum);

/* sigprocmask() how values */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);

#endif /* _SIGNAL_H */
