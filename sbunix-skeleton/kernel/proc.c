#include <proc.h>
#include <vfs.h>
#include <kalloc.h>
#include <riscv.h>
#include <string.h>
#include <vm.h>
#include <printk.h>
#include <exec.h>

struct proc proc[64];
struct proc *current_proc = NULL;
struct context scheduler_context;

int nextpid = 1;

void procinit(void) {
  struct proc *p;
  for(p = proc; p < &proc[64]; p++) {
      p->state = UNUSED;
      p->kstack = NULL;
  }
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void) {
  // Before jumping to user mode, we MUST set satp to the process's page table
  // and set sscratch to the kernel stack.
  w_satp(MAKE_SATP(current_proc->pagetable));
  sfence_vma();
  
  // Set sstatus for user mode
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP;
  x |= SSTATUS_SPIE | SSTATUS_SUM;
  w_sstatus(x);

  asm volatile("csrw sscratch, %0" : : "r" ((char *)current_proc->kstack + PAGE_SIZE));

  trap_restore(current_proc->trapframe);
}

/* Open /dev/console via VFS and assign to the next free ofile slot.
 * Returns the fd number or -1 on failure. */
static int kern_open_console(void) {
  const char *relpath;
  struct vfs_ops *ops = vfs_lookup_mount("/dev/console", &relpath);
  if (!ops || !ops->open) return -1;
  void *priv = NULL;
  int ret = ops->open(relpath, 2 /* O_RDWR */, &priv);
  if (ret < 0) return ret;
  struct open_file *f = vfs_alloc_file();
  if (!f) {
    if (ops->close) ops->close(priv);
    return -12; /* -ENOMEM */
  }
  f->ops = ops;
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
  if (ops->close) ops->close(priv);
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

  for(p = proc; p < &proc[64]; p++) {
    if(p->state == UNUSED) {
      goto found;
    }
  }
  return 0;

found:
  p->pid = nextpid++;
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
  // We'll set the name for debugging.
  const char *name = "init";
  for(int i=0; i<5; i++) p->name[i] = name[i];
}

// Wake any process sleeping on UART input (waiting_for_uart==1 only).
// Do NOT wake timed-sleep processes (waiting_for_uart==2).
void uart_wakeup(void) {
  struct proc *p;
  for (p = proc; p < &proc[64]; p++) {
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

    for(p = proc; p < &proc[64]; p++) {
      if(p->state == RUNNABLE) {
        p->state = RUNNING;
        current_proc = p;

        /* Disable interrupts before switching to user process context.
         * The user process may resume inside trap_handler (e.g., from a
         * sleep in SYS_read).  Interrupts must be off there to avoid
         * re-entrant trap corruption.  The user's sret will restore SIE
         * from sstatus.SPIE when it actually returns to user mode. */
        w_sstatus(r_sstatus() & ~SSTATUS_SIE);

        /* Always switch to this process's page table before resuming it.
         * This is required for both new processes (which also go through
         * forkret) and existing processes waking from a kernel sleep.
         * Without this, satp may still point to the last process that
         * ran through forkret (e.g. a forked child), causing the resumed
         * process to execute with the wrong address space. */
        w_satp(MAKE_SATP(p->pagetable));
        sfence_vma();

        /* Point sscratch at this process's kernel stack top so that
         * trap.S can correctly identify user-mode vs kernel-mode traps. */
        asm volatile("csrw sscratch, %0"
                     : : "r" ((char *)p->kstack + PAGE_SIZE));

        /* Ensure SUM is set so kernel can access user-mapped pages. */
        unsigned long s = r_sstatus();
        s |= SSTATUS_SUM;
        s &= ~SSTATUS_SPP;   /* return to user mode on sret */
        s |= SSTATUS_SPIE;   /* enable interrupts on sret */
        w_sstatus(s);

        swtch(&scheduler_context, &p->context);

        current_proc = 0;

        /* Re-clear sscratch now that we are back in scheduler context. */
        asm volatile("csrw sscratch, zero");
      }
    }
  }
}
