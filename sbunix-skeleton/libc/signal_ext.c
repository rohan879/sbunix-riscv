#include <signal.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>

long syscall(long num, ...);

#ifndef SIG_HOLD
#define SIG_HOLD ((void (*)(int))2)
#endif

int killpg(pid_t pgrp, int sig) {
  if (pgrp <= 0) {
    errno = EINVAL;
    return -1;
  }
  return kill(-pgrp, sig);
}

void psignal(int sig, const char *s) {
  if (s && *s)
    fprintf(stderr, "%s: %s\n", s, strsignal(sig));
  else
    fprintf(stderr, "%s\n", strsignal(sig));
}

void psiginfo(const siginfo_t *pinfo, const char *s) {
  if (!pinfo)
    return;
  if (s && *s)
    fprintf(stderr, "%s: %s\n", s, strsignal(pinfo->si_signo));
  else
    fprintf(stderr, "%s\n", strsignal(pinfo->si_signo));
}

int sig2str(int sig, char *str) {
  if (!str) {
    errno = EINVAL;
    return -1;
  }
  const char *name;
  switch (sig) {
    case SIGHUP: name = "HUP"; break;
    case SIGINT: name = "INT"; break;
    case SIGQUIT: name = "QUIT"; break;
    case SIGILL: name = "ILL"; break;
    case SIGABRT: name = "ABRT"; break;
    case SIGFPE: name = "FPE"; break;
    case SIGKILL: name = "KILL"; break;
    case SIGSEGV: name = "SEGV"; break;
    case SIGPIPE: name = "PIPE"; break;
    case SIGALRM: name = "ALRM"; break;
    case SIGTERM: name = "TERM"; break;
    case SIGUSR1: name = "USR1"; break;
    case SIGUSR2: name = "USR2"; break;
    default:
      errno = EINVAL;
      return -1;
  }
  strncpy(str, name, 31);
  str[31] = '\0';
  return 0;
}

int str2sig(const char *str, int *sig) {
  if (!str || !sig) {
    errno = EINVAL;
    return -1;
  }
  struct map { const char *name; int sig; };
  static const struct map maps[] = {
    {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"ILL", SIGILL},
    {"ABRT", SIGABRT}, {"FPE", SIGFPE}, {"KILL", SIGKILL},
    {"SEGV", SIGSEGV}, {"PIPE", SIGPIPE}, {"ALRM", SIGALRM},
    {"TERM", SIGTERM}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2},
  };
  for (size_t i = 0; i < sizeof(maps)/sizeof(maps[0]); i++) {
    if (strcmp(str, maps[i].name) == 0 || strcmp(str, strsignal(maps[i].sig)) == 0) {
      *sig = maps[i].sig;
      return 0;
    }
  }
  errno = EINVAL;
  return -1;
}

int sigqueue(pid_t pid, int sig, const union sigval value) {
  /* BF: Use SYS_rt_sigqueueinfo instead of kill() so that the kernel
   * receives the full siginfo_t (including si_value) and can properly
   * deliver queued signals per POSIX. */
  siginfo_t info;
  memset(&info, 0, sizeof(info));
  info.si_signo = sig;
  /* BF: si_code must be SI_QUEUE (-1) per POSIX.  The old code used 0
   * (SI_USER) and discarded si_value with (void)value, so sigtimedwait
   * tests that check si_value always got zero. */
  info.si_code = -1; /* SI_QUEUE */
  info.si_pid = getpid();
  info.si_uid = 0;
  memcpy(&info.si_value, &value, sizeof(value));
  long ret = syscall(SYS_rt_sigqueueinfo, (long)pid, (long)sig, &info);
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return 0;
}

int sigtimedwait(const sigset_t *set, siginfo_t *info,
                 const struct timespec *timeout) {
  long ret = syscall(SYS_rt_sigtimedwait, set, info, timeout, sizeof(sigset_t));
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

int sigwaitinfo(const sigset_t *set, siginfo_t *info) {
  long ret = syscall(SYS_rt_sigtimedwait, set, info, (void *)0, sizeof(sigset_t));
  if (ret < 0) {
    errno = (int)(-ret);
    return -1;
  }
  return (int)ret;
}

int sigpause(int sig) {
  sigset_t set;
  if (sig < 1 || sig >= NSIG) {
    errno = EINVAL;
    return -1;
  }
  if (sigprocmask(SIG_BLOCK, (const sigset_t *)0, &set) < 0) {
    return -1;
  }
  sigdelset(&set, sig);
  return sigsuspend(&set);
}

int sigignore(int sig) {
  return (signal(sig, SIG_IGN) == SIG_ERR) ? -1 : 0;
}

int sighold(int sig) {
  sigset_t set;
  if (sigemptyset(&set) < 0)
    return -1;
  if (sigaddset(&set, sig) < 0)
    return -1;
  return sigprocmask(SIG_BLOCK, &set, (sigset_t *)0);
}

int sigrelse(int sig) {
  sigset_t set;
  if (sigemptyset(&set) < 0)
    return -1;
  if (sigaddset(&set, sig) < 0)
    return -1;
  return sigprocmask(SIG_UNBLOCK, &set, (sigset_t *)0);
}

void (*sigset(int sig, void (*disp)(int)))(int) {
  struct sigaction sa;
  struct sigaction old;
  sigset_t set;

  if (disp == SIG_HOLD) {
    sigset_t oldset;
    if (sigaction(sig, (const struct sigaction *)0, &old) < 0)
      return SIG_ERR;
    if (sigemptyset(&set) < 0)
      return SIG_ERR;
    if (sigaddset(&set, sig) < 0)
      return SIG_ERR;
    if (sigprocmask(SIG_BLOCK, &set, &oldset) < 0)
      return SIG_ERR;
    return sigismember(&oldset, sig) == 1 ? SIG_HOLD : old.sa_handler;
  }

  if (sigemptyset(&sa.sa_mask) < 0)
    return SIG_ERR;
  sa.sa_flags = SA_RESTART;
  sa.sa_handler = disp;
  sa.sa_restorer = (void (*)(void))0;
  if (sigaction(sig, &sa, &old) < 0)
    return SIG_ERR;

  if (sigemptyset(&set) < 0)
    return old.sa_handler;
  if (sigaddset(&set, sig) < 0)
    return old.sa_handler;
  if (sigprocmask(SIG_UNBLOCK, &set, (sigset_t *)0) < 0)
    return SIG_ERR;

  return old.sa_handler;
}

/* ENH: Added pthread_kill and pthread_sigmask stubs.  POSIX requires
 * these to be available from <signal.h>.  Since SBUnix is single-threaded,
 * pthread_kill delegates to kill() and pthread_sigmask delegates to
 * sigprocmask(). */
int pthread_kill(pthread_t thread, int sig) {
  if (sig < 0 || sig >= NSIG)
    return EINVAL;
  if (thread != 0 && thread != (pthread_t)getpid())
    return ESRCH;
  return kill(getpid(), sig) == 0 ? 0 : errno;
}

int pthread_sigmask(int how, const sigset_t *restrict set,
                    sigset_t *restrict oldset) {
  return sigprocmask(how, set, oldset);
}
