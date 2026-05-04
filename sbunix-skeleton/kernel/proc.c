#include <proc.h>
#include <vfs.h>
#include <kalloc.h>
#include <riscv.h>
#include <string.h>
#include <vm.h>
#include <printk.h>
#include <exec.h>
#include <user_clock.h>

struct proc proc[NPROC];
struct proc *current_proc = NULL;
struct context scheduler_context;

_Static_assert(sizeof(struct trapframe) == 288, "trapframe size mismatch with trap.S");
_Static_assert(__builtin_offsetof(struct trapframe, t0) == 8, "TF_T0 mismatch");
_Static_assert(__builtin_offsetof(struct trapframe, t1) == 16, "TF_T1 mismatch");
_Static_assert(__builtin_offsetof(struct trapframe, t6) == 56, "TF_T6 mismatch");
_Static_assert(__builtin_offsetof(struct trapframe, sp) == 240, "TF_SP mismatch");
_Static_assert(__builtin_offsetof(struct trapframe, epc) == 248, "TF_EPC mismatch");
_Static_assert(__builtin_offsetof(struct trapframe, saved_sstatus) == 256,
               "TF_SSTATUS mismatch");
_Static_assert(__builtin_offsetof(struct trapframe, return_to_user) == 264,
               "TF_RETURN_TO_USER mismatch");
_Static_assert(__builtin_offsetof(struct trapframe, user_satp) == 272,
               "TF_USER_SATP mismatch");

int nextpid = 1;

void procinit(void) {
  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++) {
      p->state = UNUSED;
      p->kstack = NULL;
  }
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void) {
    /* Set stvec to kernel_trap_handler for traps during kernel execution */
    extern char kernel_trap_handler[];
    asm volatile("csrw stvec, %0" : : "r"(kernel_trap_handler));

    /* Set sstatus: clear SPP (return to user mode), set SPIE (enable interrupts) */
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP;
    x |= SSTATUS_SPIE;
    w_sstatus(x);

    current_proc->trapframe->saved_sstatus = x;
    current_proc->trapframe->return_to_user = 1;
    current_proc->trapframe->user_satp = MAKE_SATP(current_proc->pagetable);

    /* BF: Place trapframe immediately below kstack_top so trap_restore can
     * derive the next user-trap sscratch value from the frame size. */
    uint64_t kstack_top = (uint64_t)current_proc->kstack
                         + (uint64_t)current_proc->kstack_npages * PAGE_SIZE;
    struct trapframe *kstack_tf = (struct trapframe *)(kstack_top - sizeof(struct trapframe));
    memcpy(kstack_tf, current_proc->trapframe, sizeof(struct trapframe));
    trap_restore(kstack_tf);
}

/* Open /dev/tty via VFS and assign to the next free ofile slot.
 * Returns the fd number or -1 on failure. */
static int kern_open_console(void) {
  const char *relpath;
  struct mount *mnt = vfs_lookup_mount_entry("/dev/tty", &relpath);
  struct vfs_ops *ops = mnt ? mnt->ops : NULL;
  if (!ops || !ops->open) return -1;
  void *priv = NULL;
  int ret = ops->open(mnt->fs_data, relpath, 2 /* O_RDWR */, &priv);
  if (ret < 0) return ret;
  struct open_file *f = vfs_alloc_file();
  if (!f) {
    if (ops->close) ops->close(mnt->fs_data, priv);
    return -12; /* -ENOMEM */
  }
  f->ops = ops;
  f->fs_data = mnt->fs_data;
  f->private = priv;
  f->offset = 0;
  f->flags = 2; /* O_RDWR */
  f->is_pipe = 0;
  /* BF: Kernel-opened stdio fds bypass sys_open(), so record the path here
   * for /proc/self/fd and ttyname() to report the terminal correctly. */
  memcpy(f->path, "/dev/tty", 9);
  /* Find lowest free fd slot */
  for (int i = 0; i < NOFILE; i++) {
    if (current_proc->ofile[i] == NULL) {
      current_proc->ofile[i] = f;
      return i;
    }
  }
  /* No free slot */
  if (ops->close) ops->close(mnt->fs_data, priv);
  vfs_free_file(f);
  return -1;
}

// Helper to start the first process
void init_starter(void) {
    int ret = exec("./bin/init");
    if (ret < 0) {
        printk("init_starter: /bin/init not found, falling back to sbush\n");
        ret = exec("./bin/sbush");
    }
    if (ret < 0)
        printk("init_starter: exec failed\n");

    /* Open /dev/console three times for stdin(0), stdout(1), stderr(2).
     * Each gets its own open_file with independent refcount. */
    for (int i = 0; i < 3; i++) {
        int fd = kern_open_console();
        if (fd < 0)
            printk("init_starter: failed to open /dev/console for fd %d\n", i);
    }

    // Now we must jump to user mode!
    forkret();
}

// Look in the process table for an UNUSED proc.
struct proc* allocproc(void) {
  struct proc *p;
  int used_count = 0;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p->state != UNUSED)
      used_count++;
    if(p->state == UNUSED) {
      goto found;
    }
  }
  /* Process table exhausted */
  track_proc_exhaustion();
  return 0;

found:
  track_proc_alloc(used_count + 1);
  p->pid = nextpid++;
  p->pgid = p->pid;
  p->sid = p->pid;
  p->state = SLEEPING;
  p->waiting_for_uart = PROC_WAIT_NONE;
  p->sleep_until = 0;
  p->exit_status = 0;
  p->resources_released = 0;
  p->pending_terminate = 0;
  p->terminate_signal = 0;
  p->uid = 0;
  p->gid = 0;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  memset(p->trapframe, 0, PAGE_SIZE);

  /* Allocate 2 contiguous pages (8KB) for the kernel stack.  A single
   * 4KB stack is not a supported fallback: deep syscall and VFS paths can
   * corrupt the trapframe before the fault is diagnosable. */
  p->kstack = kalloc_contiguous(KSTACK_PAGES);
  if (p->kstack) {
    p->kstack_npages = KSTACK_PAGES;
  }
  if (!p->kstack) {
    kfree(p->trapframe);
    p->state = UNUSED;
    return 0;
  }

  p->user_clock = user_clock_create();
  if (!p->user_clock) {
    kfree_contiguous(p->kstack, p->kstack_npages);
    kfree(p->trapframe);
    p->kstack = NULL;
    p->trapframe = NULL;
    p->state = UNUSED;
    return 0;
  }
  p->cpu_start_ticks = 0;
  p->cputime_ticks = 0;

  /* Use kernel page table initially; exec() will install a user page table.
   * Kernel contexts always run under kernel_pagetable, and trap_restore
   * loads the process satp only for the final return to U-mode. */
  p->pagetable = kernel_pagetable;
  p->initial_brk = 0;

  // Set up new context to start executing at forkret
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64_t)forkret;
  /* BF: sp points to TOP of the (potentially multi-page) kernel stack */
  p->context.sp = (uint64_t)p->kstack + (uint64_t)p->kstack_npages * PAGE_SIZE;

  // Initialize open file table
  for (int i = 0; i < NOFILE; i++)
    p->ofile[i] = NULL;
  memset(p->fd_flags, 0, sizeof(p->fd_flags));

  memset(p->name, 0, sizeof(p->name));
  p->cwd[0] = '/';
  p->cwd[1] = '\0';
  p->exe_path[0] = '\0';
  p->cmdline[0] = '\0';
  p->cmdline_len = 0;

  // Initialize VMA list and mmap base
  p->vma_list = NULL;
  p->mmap_base = 0; /* lazily set on first mmap call */
  p->stack_top = 0;
  p->stack_bottom = 0;

  // Initialize signal handling fields
  memset(p->sigactions, 0, sizeof(p->sigactions));
  memset(p->sig_sender, 0, sizeof(p->sig_sender));
  memset(p->sigqueue_buf, 0, sizeof(p->sigqueue_buf));
  p->sigqueue_seq = 0;
  p->sigmask = 0;
  p->sigpending = 0;
  p->alarm_ticks = 0;
  p->killed_by_signal = 0;
  p->stopped_signal = 0;
  p->stop_reported = 0;
  p->continued_signal = 0;

  /* BF: Initialize alternate signal stack to SS_DISABLE.  Without this,
   * recycled process slots retain stale ss_flags from the previous
   * occupant, and sigaltstack(NULL, &old) reports wrong flags. */
  p->ss_sp = 0;
  p->ss_size = 0;
  p->ss_flags = 2; /* SS_DISABLE */

  /* BF: Zero epoch to prevent stale values from recycled process slots
   * affecting ppoll epoch detection.  sig_sender is zero-initialized
   * by memset in allocproc's struct zeroing. */
  p->signal_handled_epoch = 0;

  // Initialize POSIX timers
  for (int i = 0; i < NPTIMER; i++)
    p->timers[i].used = 0;
  /* OPT: Reset active timer count for recycled process slots */
  p->active_timer_count = 0;

  // Initialize exec tracking (for POSIX setpgid restrictions)
  p->has_execed = 0;

  // Initialize clone/thread sharing fields
  p->clone_flags = 0;
  p->shared_pt = NULL;
  p->shared_ft = NULL;
  p->shared_sa = NULL;

  /* BF: Initialize waiting_for_child so recycled slots don't carry
   * stale values from previous process occupants. */
  p->waiting_for_child = 0;
  p->waiting_for_vfork = 0;

  /* BF: Initialize deferred sigmask restore fields so recycled slots
   * don't carry stale saved_sigmask from previous occupants.  Without
   * this, sigsuspend/ppoll could embed a stale mask in signal frames. */
  p->has_saved_sigmask = 0;
  p->saved_sigmask_restore = 0;

  /* BF: Initialize in_copy_fault flag for recycled process slots.
   * Used by copyin/copyout fault path to distinguish user-address
   * faults from true kernel bugs. */
  p->in_copy_fault = 0;

  return p;
}

// Set up first user process.
void userinit(void) {
  struct proc *p;

  p = allocproc();
  
  // Manually set up the first process to run init_starter
  // which will then call exec.
  p->context.ra = (uint64_t)init_starter;
  
  p->state = RUNNABLE;
  p->umask_val = 0022;
  // We'll set the name for debugging.
  const char *name = "init";
  for(int i=0; i<5; i++) p->name[i] = name[i];
  memcpy(p->cmdline, "init", 5);
  p->cmdline_len = 5;
}

// Wake any process sleeping on UART input (waiting_for_uart==1 only).
// Do NOT wake timed-sleep processes (waiting_for_uart==2).
void uart_wakeup(void) {
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++) {
    /* BF: Skip wait4 sleepers — UART input is irrelevant for processes
     * blocked in waitpid; waking them causes spurious wakeups. */
    if (p->state == SLEEPING && p->waiting_for_uart == PROC_WAIT_UART && !p->waiting_for_child) {
      p->waiting_for_uart = PROC_WAIT_NONE;
      p->state = RUNNABLE;
    }
  }
}

void scheduler(void) {
  struct proc *p;

  /* Ensure sscratch = 0 so that any interrupt taken while we are in the
   * scheduler is recognised as a kernel-mode trap by trap.S. */
  asm volatile("csrw sscratch, zero");

  for(;;){
    /* Enable supervisor interrupts so that UART/timer IRQs can fire
     * while we are spinning looking for a RUNNABLE process.
     * sstatus.SIE was cleared when the last user ecall trapped into the
     * kernel; we must re-enable it here or UART wakeups can never arrive. */
    w_sstatus(r_sstatus() | SSTATUS_SIE);

    int found = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      if(p->state == RUNNABLE) {
        found = 1;

        /* BF: Disable interrupts BEFORE publishing RUNNING/current_proc
         * to close the race window where a timer interrupt could see
         * RUNNING but sscratch/satp/context aren't configured yet. */
        w_sstatus(r_sstatus() & ~SSTATUS_SIE);

        p->state = RUNNING;
        current_proc = p;
        user_clock_start(p, r_time());

        /* BF: Keep sscratch zero while resuming kernel C contexts.  A
         * nonzero value means "trap came from user" to trap.S; installing it
         * here lets timer interrupts overwrite the live kernel stack.  The
         * final user-return path sets sscratch immediately before sret. */
        asm volatile("csrw sscratch, zero");

        swtch(&scheduler_context, &p->context);

                user_clock_stop(p, r_time());

        current_proc = 0;

        /* Re-clear sscratch now that we are back in scheduler context. */
        asm volatile("csrw sscratch, zero");
      }
    }

    /* No RUNNABLE process found — halt until next interrupt (timer, UART).
     * Interrupts are already enabled (SSTATUS_SIE set above). */
    if (!found)
      asm volatile("wfi");
  }
}
