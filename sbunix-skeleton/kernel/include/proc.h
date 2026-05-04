#ifndef _PROC_H
#define _PROC_H

#include <riscv.h>
#include <string.h>
#include <vfs.h>
#include <user_clock.h>
#include <errno.h>

/* BF: Kernel stack is 2 contiguous pages (8KB).  A single 4KB page
 * overflows during deep syscall call chains (ext2 I/O path), corrupting
 * return addresses and causing instruction page faults. */
#define KSTACK_PAGES 2
#define KSTACK_SIZE  (KSTACK_PAGES * PAGE_SIZE)

// Saved registers for kernel context switches.
struct context {
  uint64_t ra;
  uint64_t sp;

  // Callee-saved
  uint64_t s0;
  uint64_t s1;
  uint64_t s2;
  uint64_t s3;
  uint64_t s4;
  uint64_t s5;
  uint64_t s6;
  uint64_t s7;
  uint64_t s8;
  uint64_t s9;
  uint64_t s10;
  uint64_t s11;
};

// Trapframe: user registers plus trap-entry provenance.
// Offsets must match kernel/trap.S.
struct trapframe {
  /*   0 */ uint64_t ra;
  /*   8 */ uint64_t t0;
  /*  16 */ uint64_t t1;
  /*  24 */ uint64_t t2;
  /*  32 */ uint64_t t3;
  /*  40 */ uint64_t t4;
  /*  48 */ uint64_t t5;
  /*  56 */ uint64_t t6;
  /*  64 */ uint64_t a0;
  /*  72 */ uint64_t a1;
  /*  80 */ uint64_t a2;
  /*  88 */ uint64_t a3;
  /*  96 */ uint64_t a4;
  /* 104 */ uint64_t a5;
  /* 112 */ uint64_t a6;
  /* 120 */ uint64_t a7;
  /* 128 */ uint64_t s0;
  /* 136 */ uint64_t s1;
  /* 144 */ uint64_t s2;
  /* 152 */ uint64_t s3;
  /* 160 */ uint64_t s4;
  /* 168 */ uint64_t s5;
  /* 176 */ uint64_t s6;
  /* 184 */ uint64_t s7;
  /* 192 */ uint64_t s8;
  /* 200 */ uint64_t s9;
  /* 208 */ uint64_t s10;
  /* 216 */ uint64_t s11;
  /* 224 */ uint64_t gp;
  /* 232 */ uint64_t tp;
  /* 240 */ uint64_t sp;
  /* 248 */ uint64_t epc;
  /* 256 */ uint64_t saved_sstatus;
  /* 264 */ uint64_t return_to_user;
  /* 272 */ uint64_t user_satp;
  /* 280 */ uint64_t reserved0;
};

struct signal_frame {
  uint64_t saved_sigmask;
  struct trapframe saved_tf;
};

enum procstate { UNUSED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

/* Maximum open file descriptors per process */
#define NOFILE 64

/* Forward declarations */
struct vm_area;
struct open_file;

/* POSIX timer entry */
#define NPTIMER 32  /* max POSIX timers per process */
struct posix_timer {
  int used;                    /* 1 if this timer slot is active */
  int clockid;                 /* CLOCK_REALTIME, CLOCK_MONOTONIC, etc. */
  uint64_t interval_ticks;     /* 0 for one-shot, >0 for repeating */
  uint64_t next_expiry_ticks;  /* absolute tick when timer next fires */
  int signo;                   /* signal to deliver on expiry */
  int overrun;                 /* overrun count (for periodic timers) */
};

/* Signal action descriptor (defined here so shared_sigactions can use it) */
struct kern_sigaction {
  uint64_t handler;   /* sa_handler function pointer */
  uint64_t mask;      /* sa_mask */
  int      flags;     /* sa_flags */
  uint64_t restorer;  /* sa_restorer */
};

/* Shared-resource wrappers for CLONE_VM / CLONE_FILES / CLONE_SIGHAND */
struct shared_pagetable {
  pagetable_t pt;         /* the Sv39 page table root */
  int         refcount;   /* number of procs sharing this pagetable */
  struct user_clock_state *clock; /* borrowed CLONE_VM CPU-clock alias */
};

struct shared_ftable {
  struct open_file *ofile[NOFILE]; /* the fd array */
  int               refcount;     /* number of procs sharing this table */
};

struct shared_sigactions {
  struct kern_sigaction actions[65]; /* one per signal (1-64, index 0 unused) */
  int                  refcount;    /* number of procs sharing */
};

/* Signal constants (POSIX + Linux) */
#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
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
#define SIGPWR    30
#define SIGSYS    31

// Per-process state
struct proc {
  enum procstate state;        // Process state
  pid_t pid;                   // Process ID
  pid_t pgid;                  // Process group ID
  pid_t sid;                   // Session ID
  struct proc *parent;         // Parent process
  void *kstack;                // Virtual address of kernel stack
  uint64_t sz;                 // Size of process memory (bytes)
  uint64_t initial_brk;        // lowest legal program break after exec
  pagetable_t pagetable;       // User page table
  struct trapframe *trapframe; // data page for trampoline.S
  struct context context;      // swtch() here to run process
  char name[16];               // Process name (debugging)
  int waiting_for_uart;        // 1 if sleeping for UART input; 2 if timed sleep
  uint64_t sleep_until;        // for timed sleep: wake when r_time() >= this deadline
  unsigned long alarm_ticks;   // tick count when alarm fires (0 = no alarm)
  int exit_status;             // exit status (for wait4)
  int killed_by_signal;        // signal number if killed by signal, 0 otherwise
  /* BF: Exit cleanup must complete before a process becomes reappable.
   * This flag lets wait paths reject zombies that skipped address-space,
   * fd, or shared-resource teardown instead of masking leaks or double-free
   * paths by freeing only the kernel stack/trapframe. */
  int resources_released;
  /* BF: External fatal signals cannot safely free another process's user
   * address space from the sender's syscall context.  These fields request
   * termination and force the target to run cleanup on its own kernel stack. */
  int pending_terminate;
  int terminate_signal;
  char cwd[256];               // current working directory
  char exe_path[256];          // path of executable (set by exec)
  char cmdline[256];           // NUL-separated argv image for /proc/<pid>/cmdline
  uint16_t cmdline_len;        // valid bytes in cmdline[]
  unsigned int umask_val;       // file creation mask
  struct open_file *ofile[NOFILE]; // open file descriptors (VFS)
  uint8_t fd_flags[NOFILE]; /* Per-fd flags: FD_CLOEXEC=1, FD_CLOFORK=2 */
  /* VMA list for tracking all user memory regions */
  struct vm_area *vma_list;    // sorted linked list of virtual memory areas
  uint64_t mmap_base;          // top-down search hint for non-MAP_FIXED mmap
  /* signal handling */
  struct kern_sigaction sigactions[65]; /* one per signal (1-64, index 0 unused) */
  uint64_t sigmask;             /* blocked signal mask */
  uint64_t sigpending;          /* pending signal bitmask */
  uint64_t ss_sp;               /* alternate signal stack base */
  uint64_t ss_size;             /* alternate signal stack size */
  int      ss_flags;            /* SS_DISABLE=2, SS_ONSTACK=1, 0=inactive */
  /* stack bounds for auto-grow */
  uint64_t stack_top;            /* top of user stack (highest VA) */
  uint64_t stack_bottom;         /* lowest allocated stack page */
  int has_execed;                           /* set to 1 after execve (for setpgid) */
  int stopped_signal;                       /* non-zero if stopped by signal (for WUNTRACED) */
  /* Clone/thread sharing state */
  int clone_flags;                          /* flags this proc was created with */
  struct shared_pagetable  *shared_pt;      /* non-NULL if CLONE_VM */
  struct shared_ftable     *shared_ft;      /* non-NULL if CLONE_FILES */
  struct shared_sigactions *shared_sa;      /* non-NULL if CLONE_SIGHAND */
  struct user_clock_state  *user_clock;     /* read-only userspace CPU clock */
  uint64_t cpu_start_ticks;                 /* rdtime when this proc was scheduled */
  /* futex support */
  uint64_t futex_addr;          /* VA being waited on (0 = not waiting) */
  /* SA_RESTART support: saved syscall args for restart after signal */
  int in_syscall;               /* 1 while inside a blocking syscall */
  uint64_t saved_a0;
  uint64_t saved_a1;
  uint64_t saved_a2;
  uint64_t saved_a3;
  uint64_t saved_a4;
  uint64_t saved_a5;
  /* POSIX timers */
  struct posix_timer timers[NPTIMER];

  /* BF: Track kernel stack size for proper free on process exit.
   * Added at the END of struct to avoid shifting field offsets. */
  int kstack_npages;

  /* BF: Legacy counter kept for compatibility with older diagnostics.
   * The authoritative CPU clock now lives in user_clock and uses rdtime
   * deltas so sleep does not count as process CPU time. */
  uint64_t cputime_ticks;

  /* BF: Deferred sigmask restore for ppoll/pselect6.  When these syscalls
   * return -EINTR, the temporary mask must stay active so deliver_signal
   * can deliver the handler.  sigreturn restores saved_sigmask_restore. */
  uint64_t saved_sigmask_restore;
  int      has_saved_sigmask;

  /* BF: Per-signal sender metadata for SA_SIGINFO delivery.
   * Captured at generation time, consumed at delivery time.
   * Standard signals coalesce per POSIX — first sender's metadata is kept.
   * Replaces the old sigchld_code field with per-signal granularity. */
  struct {
    int32_t si_pid;      /* sender PID or child PID (for SIGCHLD) */
    int32_t si_uid;      /* sender UID */
    int32_t si_code;     /* K_SI_USER, K_SI_TIMER, K_CLD_STOPPED, etc. */
    int32_t si_status;   /* SIGCHLD: exit code (CLD_EXITED) or stop signal (CLD_STOPPED) */
    uint8_t valid;
  } sig_sender[65];  /* index 0 unused (signal numbers 1-64) */

  /* BF: Epoch counter incremented each time deliver_signal dispatches a
   * user handler.  ppoll/pselect6 snapshot this before the mask swap
   * to detect signals handled between raise() and ppoll(). */
  uint64_t signal_handled_epoch;

  /* BF: Per-process UID/GID for kill() permission checks.
   * Default 0 (root) in single-user OS. */
  int uid;
  int gid;

  /* BF: Set to 1 while sleeping in sys_wait4 (waitpid/wait4).  Allows
   * setpgid to wake only wait4 sleepers without disturbing pipe/poll/
   * futex sleepers that also have waiting_for_uart unset. */
  int waiting_for_child;

  /* BF: Parent blocks here for Linux-style vfork until the child either
   * execs successfully or exits, preventing concurrent use of the shared mm. */
  pid_t waiting_for_vfork;

  /* BF: Per-process signal info queue for rt_sigqueueinfo / sigtimedwait.
   * Stores the siginfo_t provided by sigqueue so that sigtimedwait can
   * retrieve si_value and si_code.  Without this, queued signal data
   * was discarded and sigtimedwait returned empty siginfo. */
  #define NSIGQUEUE 64
  struct {
    int valid;
    int signum;
    uint64_t seq;
    uint8_t info[128]; /* raw siginfo_t bytes */
  } sigqueue_buf[NSIGQUEUE];
  uint64_t sigqueue_seq;

  /* BF: Flag set by copyin/copyout fault path to distinguish
   * user-address faults from true kernel bugs during copy operations.
   * Must be initialized to 0 in allocproc for recycled process slots. */
  uint8_t in_copy_fault;

  /* OPT: Count of active POSIX timers so the timer interrupt can skip
   * the NPTIMER inner loop for processes with no timers.  Maintained by
   * timer_create (++), timer_delete (--), and overrun-disarm (--).
   * uint8_t is safe: NPTIMER=32 cannot overflow a uint8_t. */
  uint8_t active_timer_count;
};

/* Accessor helpers: return the effective ofile[] and sigactions[] for a proc,
 * indirecting through shared wrappers when present. */
static inline struct open_file **proc_ofile(struct proc *p) {
  return p->shared_ft ? p->shared_ft->ofile : p->ofile;
}

static inline struct kern_sigaction *proc_sigactions(struct proc *p) {
  return p->shared_sa ? p->shared_sa->actions : p->sigactions;
}

static inline uint64_t signal_bit(int signum) {
  return 1UL << (signum - 1);
}

static inline int signal_is_valid(int signum) {
  return signum > 0 && signum < 65;
}

static inline int signal_is_realtime(int signum) {
  return signum >= 32 && signum < 65;
}

/* BF: Centralize signal deliverability so all trap-return and blocking
 * syscall paths agree that only SIGKILL and SIGSTOP bypass the process mask.
 * Job-control stop signals such as SIGTSTP remain pending while blocked. */
static inline uint64_t signal_unblockable_mask(void) {
  return signal_bit(SIGKILL) | signal_bit(SIGSTOP);
}

static inline uint64_t signal_mask_sanitize(uint64_t mask) {
  return mask & ~signal_unblockable_mask();
}

static inline uint64_t signal_deliverable_mask(struct proc *p) {
  if (!p) return 0;
  return p->sigpending & ~signal_mask_sanitize(p->sigmask);
}

/* BF: Signal info codes for kernel use (matching Linux ABI values).
 * Libc defines are unavailable in kernel, so use K_ prefixed literals. */
#define K_SI_USER       0
#define K_SI_QUEUE      (-1)
#define K_SI_TIMER      (-2)
#define K_CLD_EXITED    1
#define K_CLD_KILLED    2
#define K_CLD_STOPPED   5
#define K_CLD_CONTINUED 6

static inline int signal_queue_has_more(struct proc *p, int signum) {
    for (int i = 0; i < NSIGQUEUE; i++) {
        if (p->sigqueue_buf[i].valid && p->sigqueue_buf[i].signum == signum)
            return 1;
    }
    return 0;
}

static inline int signal_queue_enqueue_raw(struct proc *target, int signum,
                                           const uint8_t info[128]) {
    if (!signal_is_valid(signum))
        return -EINVAL;

    int slot = -1;
    for (int i = 0; i < NSIGQUEUE; i++) {
        if (!target->sigqueue_buf[i].valid) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -EAGAIN;

    target->sigqueue_seq++;
    if (target->sigqueue_seq == 0)
        target->sigqueue_seq = 1;
    target->sigqueue_buf[slot].signum = signum;
    target->sigqueue_buf[slot].seq = target->sigqueue_seq;
    memcpy(target->sigqueue_buf[slot].info, info, 128);
    target->sigqueue_buf[slot].valid = 1;
    return 0;
}

static inline int signal_queue_enqueue_fields(struct proc *target, int signum,
                                              int32_t si_pid, int32_t si_uid,
                                              int32_t si_code,
                                              int32_t si_status) {
    uint8_t info[128];
    memset(info, 0, sizeof(info));
    memcpy(info + 0, &signum, 4);
    memcpy(info + 4, &(int32_t){0}, 4);
    memcpy(info + 8, &si_code, 4);
    memcpy(info + 12, &si_pid, 4);
    memcpy(info + 16, &si_uid, 4);
    memcpy(info + 20, &si_status, 4);
    return signal_queue_enqueue_raw(target, signum, info);
}

static inline int signal_queue_consume_one(struct proc *p, int signum,
                                           uint8_t *info_out) {
    int slot = -1;
    uint64_t best_seq = 0;
    for (int i = 0; i < NSIGQUEUE; i++) {
        if (!p->sigqueue_buf[i].valid || p->sigqueue_buf[i].signum != signum)
            continue;
        if (slot < 0 || p->sigqueue_buf[i].seq < best_seq) {
            slot = i;
            best_seq = p->sigqueue_buf[i].seq;
        }
    }
    if (slot < 0)
        return 0;
    if (info_out)
        memcpy(info_out, p->sigqueue_buf[slot].info, 128);
    p->sigqueue_buf[slot].valid = 0;
    return 1;
}

static inline void signal_queue_purge_all(struct proc *p, int signum) {
    for (int i = 0; i < NSIGQUEUE; i++) {
        if (p->sigqueue_buf[i].valid && p->sigqueue_buf[i].signum == signum)
            p->sigqueue_buf[i].valid = 0;
    }
    if (signal_is_valid(signum))
        p->sig_sender[signum].valid = 0;
}

static inline int signal_default_is_ignore(int signum) {
    return signum == SIGCHLD || signum == SIGURG || signum == SIGWINCH;
}

static inline void signal_discard_pending(struct proc *p, int signum) {
    p->sigpending &= ~signal_bit(signum);
    signal_queue_purge_all(p, signum);
}

/* Return non-zero when a blocking syscall should return -EINTR so trap
 * return can deliver a user handler or default stop/terminate action.
 * Ignored, blocked, and default-ignore signals are consumed or skipped here
 * so they do not spuriously interrupt sleeps. */
static inline int signal_pending_interrupts_syscall(struct proc *p,
                                                    int wait_child) {
    uint64_t deliverable = signal_deliverable_mask(p);
    struct kern_sigaction *sa;

    if (!deliverable)
        return 0;
    sa = proc_sigactions(p);

    for (int s = 1; s < 65 && deliverable; s++) {
        if (!(deliverable & signal_bit(s)))
            continue;

        if (sa[s].handler > 1) {
            return 1;
        }
        if (sa[s].handler == 1) {
            signal_discard_pending(p, s);
            continue;
        }
        if (signal_default_is_ignore(s)) {
            signal_discard_pending(p, s);
            continue;
        }
        return 1;
    }

    return 0;
}

/* BF: Size of user-space siginfo_t for copyin/copyout to user buffers.
 * Must match libc/include/signal.h siginfo_t layout (RV64 LP64):
 *   si_signo(4) + si_errno(4) + si_code(4) + si_pid(4)
 *   + si_uid(4) + si_status(4) + si_value(8) + si_addr(8) = 40 bytes.
 * Internal kernel kinfo[128] buffers stay 128 for signal frame delivery
 * (deliver_signal allocates 128 bytes on user stack via sp -= 128);
 * only user-facing copyin/copyout must respect this size. */
#define K_SIGINFO_SIZE 40

/* BF: Set signal pending with sender metadata. Does NOT wake target.
 * All sig_sender and sigqueue_buf accesses are safe without locking
 * because SBUnix is a single-hart kernel with no preemption in S-mode. */
static inline void sig_set_pending(struct proc *target, int sig,
                                   int32_t si_pid, int32_t si_uid,
                                   int32_t si_code) {
    if (!signal_is_valid(sig)) return;
    if (signal_is_realtime(sig)) {
        /* BF: Realtime signals queue every generated instance.  If the
         * fixed teaching-OS queue is full, leave the pending bit and
         * sig_sender fallback below so kill/timer delivery still wakes
         * the target instead of silently losing the signal. */
        (void)signal_queue_enqueue_fields(target, sig, si_pid, si_uid,
                                          si_code, 0);
    }
    /* BF: Only store metadata if signal is not already pending —
     * standard signals coalesce per POSIX, first sender's metadata kept. */
    if (!(target->sigpending & signal_bit(sig))) {
        target->sig_sender[sig].si_pid = si_pid;
        target->sig_sender[sig].si_uid = si_uid;
        target->sig_sender[sig].si_code = si_code;
        target->sig_sender[sig].si_status = 0;
        target->sig_sender[sig].valid = 1;
    }
    target->sigpending |= signal_bit(sig);
}

/* BF: Extended version with si_status for SIGCHLD */
static inline void sig_set_pending_ex(struct proc *target, int sig,
                                      int32_t si_pid, int32_t si_uid,
                                      int32_t si_code, int32_t si_status) {
    if (!signal_is_valid(sig)) return;
    if (!(target->sigpending & signal_bit(sig))) {
        target->sig_sender[sig].si_pid = si_pid;
        target->sig_sender[sig].si_uid = si_uid;
        target->sig_sender[sig].si_code = si_code;
        target->sig_sender[sig].si_status = si_status;
        target->sig_sender[sig].valid = 1;
    }
    target->sigpending |= signal_bit(sig);
}

void            procinit(void);
struct proc*    allocproc(void);
void            userinit(void);
void            scheduler(void) __attribute__((noreturn));
void            forkret(void);
void            swtch(struct context*, struct context*);
void            trap_restore(struct trapframe*);
void            uart_wakeup(void);

#define NPROC 256

extern struct proc *current_proc;
extern struct proc proc[NPROC];
extern struct context scheduler_context;

#endif
