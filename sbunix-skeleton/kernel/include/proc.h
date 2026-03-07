#ifndef _PROC_H
#define _PROC_H

#include <riscv.h>

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

enum procstate { UNUSED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

/* Maximum open file descriptors per process */
#define NOFILE 16

/* Maximum anonymous mmap regions per process */
#define NMMAP 16

/* One anonymous mmap region */
struct mmap_region {
  uint64_t va;   /* base virtual address (page-aligned); 0 = unused */
  uint64_t len;  /* length in bytes (page-aligned) */
};

/* Simple file descriptor table entry */
struct file {
  int used;             // 1 if slot occupied
  int pipe_read;        // 1 if this is the read end of a pipe
  int pipe_write;       // 1 if this is the write end of a pipe
  int pipe_id;          // which pipe (index into kernel pipe table)
  /* tar file fields (used when pipe_read==0 && pipe_write==0 && used==1) */
  unsigned long tar_offset; // byte offset within file data
  unsigned long tar_size;   // total file size
  char *tar_data;           // pointer into tarfs data (after header)
};

// Per-process state
struct proc {
  enum procstate state;        // Process state
  pid_t pid;                   // Process ID
  struct proc *parent;         // Parent process
  void *kstack;                // Virtual address of kernel stack
  uint64_t sz;                 // Size of process memory (bytes)
  pagetable_t pagetable;       // User page table
  struct trapframe *trapframe; // data page for trampoline.S
  struct context context;      // swtch() here to run process
  char name[16];               // Process name (debugging)
  int waiting_for_uart;        // 1 if sleeping for UART input; 2 if timed sleep
  uint64_t sleep_until;        // for timed sleep: wake when ticks >= this value
  int exit_status;             // exit status (for wait4)
  char cwd[256];               // current working directory
  struct file ofile[NOFILE];   // open file descriptors
  /* anonymous mmap tracking */
  struct mmap_region mmap_regions[NMMAP]; // per-process mmap table
  uint64_t mmap_base;          // next VA to use for non-MAP_FIXED mmap
  /* signal handling */
  struct kern_sigaction {
    uint64_t handler;   /* sa_handler function pointer */
    uint64_t mask;      /* sa_mask */
    int      flags;     /* sa_flags */
    uint64_t restorer;  /* sa_restorer */
  } sigactions[32];             /* one per signal (indices 0-31) */
  uint64_t sigmask;             /* blocked signal mask */
  uint64_t sigpending;          /* pending signal bitmask */
};

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
