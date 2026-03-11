#ifndef _PROC_H
#define _PROC_H

#include <riscv.h>
#include <vfs.h>

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

// Trapframe matches the layout of registers saved on stack in trap.S
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
#define NOFILE 16

/* Forward declarations */
struct vm_area;
struct open_file;

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

/* Signal trampoline is mapped at this user VA in every process */
#define SIG_TRAMPOLINE_VA  0x3FFFFFF000UL  /* just below top of Sv39 user space */

/* Signal constants */
#define SIGINT     2
#define SIGQUIT    3
#define SIGKILL    9
#define SIGSEGV   11
#define SIGPIPE   13
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGWINCH  28

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
  unsigned int umask_val;       // file creation mask
  struct open_file *ofile[NOFILE]; // open file descriptors (VFS)
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
  /* Clone/thread sharing state */
  int clone_flags;                          /* flags this proc was created with */
  struct shared_pagetable  *shared_pt;      /* non-NULL if CLONE_VM */
  struct shared_ftable     *shared_ft;      /* non-NULL if CLONE_FILES */
  struct shared_sigactions *shared_sa;      /* non-NULL if CLONE_SIGHAND */
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

extern struct proc *current_proc;
extern struct proc proc[64];
extern struct context scheduler_context;

#endif
