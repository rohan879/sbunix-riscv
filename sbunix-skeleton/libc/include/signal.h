#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <sys/types.h>
#include <stddef.h>

#if !defined(__timespec_defined) && !defined(_STRUCT_TIMESPEC)
struct timespec {
  time_t tv_sec;
  long tv_nsec;
};
#define __timespec_defined
#endif

/* Signal numbers (Linux RISC-V ABI) */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPOLL   SIGIO
#define SIGPWR    30
#define SIGSYS    31
#define SIGRTMIN  32
#define SIGRTMAX  64
#define NSIG      65

#define SI_USER    0
#define SI_QUEUE   (-1)
#define SI_TIMER   (-2)
#define SI_ASYNCIO (-4)
#define SI_MESGQ   (-3)

#define CLD_EXITED    1
#define CLD_KILLED    2
#define CLD_DUMPED    3
#define CLD_TRAPPED   4
#define CLD_STOPPED   5
#define CLD_CONTINUED 6

#define BUS_ADRALN  1
#define BUS_ADRERR  2
#define BUS_OBJERR  3

#define FPE_INTDIV 1
#define FPE_INTOVF 2
#define FPE_FLTDIV 3
#define FPE_FLTOVF 4
#define FPE_FLTUND 5
#define FPE_FLTRES 6
#define FPE_FLTINV 7
#define FPE_FLTSUB 8

#define ILL_ILLOPC 1
#define ILL_ILLOPN 2
#define ILL_ILLADR 3
#define ILL_ILLTRP 4
#define ILL_PRVOPC 5
#define ILL_PRVREG 6
#define ILL_COPROC 7
#define ILL_BADSTK 8

#define SEGV_MAPERR 1
#define SEGV_ACCERR 2

#define TRAP_BRKPT 1
#define TRAP_TRACE 2

#define SIGEV_SIGNAL 0
#define SIGEV_NONE   1
#define SIGEV_THREAD 2

#define SIG2STR_MAX 32

/* Special SIG_* values */
#define SIG_DFL  ((void (*)(int))0)
#define SIG_IGN  ((void (*)(int))1)
#define SIG_ERR  ((void (*)(int))-1)
#define SIG_HOLD ((void (*)(int))2)

#ifndef __sigset_t_defined
typedef unsigned long sigset_t;
#define __sigset_t_defined
#endif

typedef volatile int sig_atomic_t;
typedef void (*sighandler_t)(int);

/* ENH: POSIX requires pthread_t and pthread_attr_t to be visible from
 * <signal.h> for pthread_kill(), pthread_sigmask(), and struct sigevent.
 * We forward-declare the types here with include guards so pthread.h
 * can share the same definitions without conflict. */
#ifndef __pthread_t_defined
typedef unsigned long pthread_t;
#define __pthread_t_defined
#endif
#ifndef __pthread_attr_t_defined
typedef struct { int __detachstate; size_t __stacksize; } pthread_attr_t;
#define __pthread_attr_t_defined
#endif

#ifndef __sigval_t_defined
typedef union sigval {
  int sival_int;
  void *sival_ptr;
} sigval_t;
#define __sigval_t_defined
#endif

struct sigevent {
  int sigev_notify;
  int sigev_signo;
  union sigval sigev_value;
  void (*sigev_notify_function)(union sigval);
  /* ENH: POSIX requires sigev_notify_attributes to be pthread_attr_t*,
   * not void*.  The os-test checks the exact type. */
  pthread_attr_t *sigev_notify_attributes;
};

/* Alternate signal stack */
#define SS_ONSTACK  1
#define SS_DISABLE  2
#define MINSIGSTKSZ 2048
#define SIGSTKSZ    8192

typedef struct {
  void *ss_sp;
  int ss_flags;
  size_t ss_size;
} stack_t;

typedef struct {
  unsigned long regs[32];
} mcontext_t;

typedef struct ucontext {
  struct ucontext *uc_link;
  sigset_t uc_sigmask;
  stack_t uc_stack;
  mcontext_t uc_mcontext;
} ucontext_t;

/* sigaction flags */
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_RESTORER  0x04000000
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

typedef struct {
  int si_signo;
  int si_errno;
  int si_code;
  pid_t si_pid;
  uid_t si_uid;
  int si_status;
  sigval_t si_value;
  void *si_addr;
} siginfo_t;

struct sigaction {
  union {
    void (*sa_handler)(int);
    void (*sa_sigaction)(int, siginfo_t *, void *);
  };
  sigset_t sa_mask;
  int sa_flags;
  void (*sa_restorer)(void);
};

void (*signal(int signum, void (*handler)(int)))(int);
int sigaction(int signum, const struct sigaction *act,
              struct sigaction *oldact);
int kill(pid_t pid, int sig);
int raise(int sig);

int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signum);
int sigdelset(sigset_t *set, int signum);
int sigismember(const sigset_t *set, int signum);

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigaltstack(const stack_t *ss, stack_t *old_ss);

int sigsuspend(const sigset_t *mask);
int sigpending(sigset_t *set);
int pause(void);
int sigwait(const sigset_t *set, int *sig);

int killpg(pid_t pgrp, int sig);
void psignal(int sig, const char *s);
void psiginfo(const siginfo_t *pinfo, const char *s);
int sig2str(int sig, char *str);
int str2sig(const char *str, int *sig);
int sigqueue(pid_t pid, int sig, const union sigval value);
int sigtimedwait(const sigset_t *set, siginfo_t *info,
                 const struct timespec *timeout);
int sigwaitinfo(const sigset_t *set, siginfo_t *info);
int sigpause(int sig);
int sigignore(int sig);
int sighold(int sig);
int sigrelse(int sig);
void (*sigset(int sig, void (*disp)(int)))(int);

/* ENH: POSIX requires pthread_kill and pthread_sigmask to be declared
 * in <signal.h> so that programs can send signals to threads and
 * manipulate per-thread signal masks without including <pthread.h>. */
int pthread_kill(pthread_t thread, int sig);
int pthread_sigmask(int how, const sigset_t *restrict set,
                    sigset_t *restrict oldset);

#endif /* _SIGNAL_H */
