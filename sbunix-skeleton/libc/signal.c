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

/*
 * kill() — send signal `sig` to process `pid`.
 * Uses SYS_kill (129) which matches the Linux RISC-V ABI.
 */
int kill(pid_t pid, int sig) {
    return (int)syscall(SYS_kill, pid, sig);
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
    return (int)syscall(SYS_rt_sigaction, signum, act, oldact,
                        sizeof(sigset_t));
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
    sa.sa_restorer = (void *)0;

    if (sigaction(signum, &sa, &old) < 0)
        return SIG_ERR;
    return old.sa_handler;
}
