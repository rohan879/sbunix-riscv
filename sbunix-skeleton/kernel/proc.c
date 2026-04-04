#include <proc.h>
#include <vfs.h>
#include <kalloc.h>
#include <riscv.h>
#include <string.h>
#include <vm.h>
#include <printk.h>
#include <exec.h>

struct proc proc[NPROC];
struct proc *current_proc = NULL;
struct context scheduler_context;

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
#ifdef __CHERI_PURE_CAPABILITY__
    uint64_t stvec_addr = (uint64_t)__builtin_cheri_address_get(
        (void *__capability)kernel_trap_handler);
    asm volatile("csrw stvec, %0" : : "r"(stvec_addr));
#else
    asm volatile("csrw stvec, %0" : : "r"(kernel_trap_handler));
#endif

    /* Set sstatus: clear SPP (return to user mode), set SPIE (enable interrupts) */
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP;
    x |= SSTATUS_SPIE;
    w_sstatus(x);

    /* Copy trapframe onto the kernel stack so trap_restore's _ret_user
     * can compute sscratch = sp + 256 = kernel stack top correctly.
     * (The persistent trapframe page is separate from the kernel stack.) */
    struct trapframe kstack_tf;
    memcpy(&kstack_tf, current_proc->trapframe, sizeof(struct trapframe));
    trap_restore(&kstack_tf);
}

/* Open /dev/console via VFS and assign to the next free ofile slot.
 * Returns the fd number or -1 on failure. */
static int kern_open_console(void) {
  const char *relpath;
  struct mount *mnt = vfs_lookup_mount_entry("/dev/console", &relpath);
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

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }

  // Allocate a kernel stack page.
  if((p->kstack = kalloc()) == 0){
    kfree(p->trapframe);
    p->state = UNUSED;
    return 0;
  }

  // Use kernel page table initially; exec() will install a user page table.
  // This ensures the scheduler can safely switch satp to this process's
  // page table before the process has run exec().
  p->pagetable = kernel_pagetable;

  // Set up new context to start executing at forkret
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64_t)forkret;
  p->context.sp = (uint64_t)p->kstack + PAGE_SIZE;

  // Initialize open file table
  for (int i = 0; i < NOFILE; i++)
    p->ofile[i] = NULL;

  // Initialize VMA list and mmap base
  p->vma_list = NULL;
  p->mmap_base = 0; /* lazily set on first mmap call */

  // Initialize signal handling fields
  memset(p->sigactions, 0, sizeof(p->sigactions));
  p->sigmask = 0;
  p->sigpending = 0;
  p->alarm_ticks = 0;
  p->killed_by_signal = 0;
  p->stopped_signal = 0;

  // Initialize POSIX timers
  for (int i = 0; i < NPTIMER; i++)
    p->timers[i].used = 0;

  // Initialize exec tracking (for POSIX setpgid restrictions)
  p->has_execed = 0;

  // Initialize clone/thread sharing fields
  p->clone_flags = 0;
  p->shared_pt = NULL;
  p->shared_ft = NULL;
  p->shared_sa = NULL;

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
}

// Wake any process sleeping on UART input (waiting_for_uart==1 only).
// Do NOT wake timed-sleep processes (waiting_for_uart==2).
void uart_wakeup(void) {
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->state == SLEEPING && p->waiting_for_uart == 1) {
      p->waiting_for_uart = 0;
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
        p->state = RUNNING;
        current_proc = p;

        /* Disable interrupts before switching to user process context.
         * The user process may resume inside trap_handler (e.g., from a
         * sleep in SYS_read).  Interrupts must be off there to avoid
         * re-entrant trap corruption.  The user's sret will restore SIE
         * from sstatus.SPIE when it actually returns to user mode. */
        w_sstatus(r_sstatus() & ~SSTATUS_SIE);

        /* Set sscratch and switch to process page table */
                asm volatile("csrw sscratch, %0" : : "r"((uint64_t)p->kstack + PAGE_SIZE));
                if (p->pagetable != kernel_pagetable) {
                    w_satp(MAKE_SATP(p->pagetable));
                    sfence_vma();
                }

        swtch(&scheduler_context, &p->context);

                /* Switch back to kernel page table */
                w_satp(MAKE_SATP(kernel_pagetable));
                sfence_vma();

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
