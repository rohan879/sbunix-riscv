#ifndef _PROC_H
#define _PROC_H

#include <riscv.h>
#include <vfs.h>

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

// Trapframe: user registers + kernel scratch area for the trampoline.
// The first 256 bytes hold saved user registers (offsets match trap.S).
// Bytes 256-279 hold kernel values loaded by the trampoline on trap entry.
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
};

struct shared_ftable {
  struct open_file *ofile[NOFILE]; /* the fd array */
  int               refcount;     /* number of procs sharing this table */
};

struct shared_sigactions {
  struct kern_sigaction actions[32]; /* one per signal */
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
  pagetable_t pagetable;       // User page table
  struct trapframe *trapframe; // data page for trampoline.S
  struct context context;      // swtch() here to run process
  char name[16];               // Process name (debugging)
  int waiting_for_uart;        // 1 if sleeping for UART input; 2 if timed sleep
  uint64_t sleep_until;        // for timed sleep: wake when r_time() >= this deadline
  unsigned long alarm_ticks;   // tick count when alarm fires (0 = no alarm)
  int exit_status;             // exit status (for wait4)
  int killed_by_signal;        // signal number if killed by signal, 0 otherwise
  char cwd[256];               // current working directory
  char exe_path[256];          // path of executable (set by exec)
  unsigned int umask_val;       // file creation mask
  struct open_file *ofile[NOFILE]; // open file descriptors (VFS)
  uint8_t fd_flags[NOFILE]; /* Per-fd flags: FD_CLOEXEC=1, FD_CLOFORK=2 */
  /* VMA list for tracking all user memory regions */
  struct vm_area *vma_list;    // sorted linked list of virtual memory areas
  uint64_t mmap_base;          // next VA to use for non-MAP_FIXED mmap
  /* signal handling */
  struct kern_sigaction sigactions[32]; /* one per signal (indices 0-31) */
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

  /* BF: Per-process CPU time in timer ticks for CLOCK_PROCESS_CPUTIME_ID.
   * Incremented by the timer interrupt handler each tick while this
   * process is current_proc.  Added at END to avoid shifting offsets. */
  uint64_t cputime_ticks;

  /* BF: Deferred sigmask restore for ppoll/pselect6.  When these syscalls
   * return -EINTR, the temporary mask must stay active so deliver_signal
   * can deliver the handler.  sigreturn restores saved_sigmask_restore. */
  uint64_t saved_sigmask_restore;
  int      has_saved_sigmask;

  /* BF: SIGCHLD si_code for SA_SIGINFO handlers.  Set by stop/continue/exit
   * paths so deliver_signal can populate siginfo_t correctly. */
  int sigchld_code;

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

  /* BF: Per-process signal info queue for rt_sigqueueinfo / sigtimedwait.
   * Stores the siginfo_t provided by sigqueue so that sigtimedwait can
   * retrieve si_value and si_code.  Without this, queued signal data
   * was discarded and sigtimedwait returned empty siginfo. */
  #define NSIGQUEUE 16
  struct {
    int valid;
    int signum;
    uint8_t info[128]; /* raw siginfo_t bytes */
  } sigqueue_buf[NSIGQUEUE];
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
  return signum > 0 && signum < 32;
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
