/*
 * signal.c — signal-related syscall wrappers for SBUnix libc.
 *
 * The kernel only reliably handles delivery of SIGCHLD (from clone/wait)
 * and uses Ctrl-C via the UART interrupt path rather than POSIX signals.
 * These wrappers provide the standard API surface so programs can compile
 * and link; the kernel may silently ignore handlers it does not support.
 */

#include <signal.h>
#include <unistd.h>
#include <syscall.h>
#include <errno.h>
#include <stddef.h>

__attribute__((naked, used))
void __signal_restorer(void) {
    asm volatile("li a7, 139\n ecall\n");
}

/*
 * kill() — send signal `sig` to process `pid`.
 * Uses SYS_kill (129) which matches the Linux RISC-V ABI.
 */
int kill(pid_t pid, int sig) {
    return (int)__syscall_ret(syscall(SYS_kill, pid, sig));
}

/*
 * raise() — send signal `sig` to the calling process.
 */
int raise(int sig) {
    return kill(getpid(), sig);
}

/*
 * Signal set manipulation helpers (operate on a bitmask).
 */
int sigemptyset(sigset_t *set) {
    *set = 0;
    return 0;
}

int sigfillset(sigset_t *set) {
    *set = ~(sigset_t)0;
    return 0;
}

int sigaddset(sigset_t *set, int signum) {
    if (signum < 1 || signum >= NSIG)
        return -1;
    *set |= ((sigset_t)1 << (signum - 1));
    return 0;
}

int sigdelset(sigset_t *set, int signum) {
    if (signum < 1 || signum >= NSIG)
        return -1;
    *set &= ~((sigset_t)1 << (signum - 1));
    return 0;
}

int sigismember(const sigset_t *set, int signum) {
    if (signum < 1 || signum >= NSIG)
        return -1;
    return (*set & ((sigset_t)1 << (signum - 1))) != 0;
}

/*
 * sigprocmask() — examine/change the calling thread's signal mask.
 * Routes through SYS_rt_sigprocmask; the kernel may not implement this.
 */
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    return (int)syscall(SYS_rt_sigprocmask, how, set, oldset, sizeof(sigset_t));
}

/*
 * sigaction() — examine/change a signal action.
 * Routes through SYS_rt_sigaction.
 */
int sigaction(int signum, const struct sigaction *act,
              struct sigaction *oldact) {
    struct sigaction kact;
    if (act) {
        kact = *act;
        kact.sa_restorer = (void *)__signal_restorer;
        return (int)__syscall_ret(syscall(SYS_rt_sigaction, signum, &kact, oldact,
                            sizeof(sigset_t)));
    }
    return (int)__syscall_ret(syscall(SYS_rt_sigaction, signum, act, oldact,
                        sizeof(sigset_t)));
}

/*
 * signal() — simplified BSD-style signal() using sigaction() underneath.
 * Returns the previous handler, or SIG_ERR on failure.
 */
void (*signal(int signum, void (*handler)(int)))(int) {
    struct sigaction sa, old;

    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handler;
    sa.sa_flags   = SA_RESTART;
    sa.sa_restorer = (void *)__signal_restorer;

    if (sigaction(signum, &sa, &old) < 0)
        return SIG_ERR;
    return old.sa_handler;
}

int sigaltstack(const stack_t *ss, stack_t *old_ss) {
    return (int)__syscall_ret(syscall(SYS_sigaltstack, ss, old_ss));
}

/*
 * sigsuspend() — temporarily replace signal mask and suspend until
 * a signal is delivered.  Always returns -1 with errno == EINTR.
 */
int sigsuspend(const sigset_t *mask) {
    errno = EINTR; // always returns -1 with EINTR
    return (int)__syscall_ret(syscall(133, mask, sizeof(sigset_t)));
}

/*
 * sigpending() — get the set of pending signals.
 */
int sigpending(sigset_t *set) {
    return (int)__syscall_ret(syscall(136, set, sizeof(sigset_t)));
}

/*
 * pause() — suspend until a signal is delivered.
 * Implemented as sigsuspend with the current mask.
 */
int pause(void) {
    sigset_t mask;
    sigprocmask(SIG_BLOCK, NULL, &mask); // get current mask
    return sigsuspend(&mask);
}

/*
 * sigwait() — wait for a signal in `set`.
 * Simple implementation: loop checking sigpending, sigsuspend if none ready.
 */
int sigwait(const sigset_t *set, int *sig) {
    int signo;

    if (!set || !sig)
        return EINVAL;

    signo = sigwaitinfo(set, (siginfo_t *)0);
    if (signo < 0)
        return errno;

    *sig = signo;
    return 0;
}
