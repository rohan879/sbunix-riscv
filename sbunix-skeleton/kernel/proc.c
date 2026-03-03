#include <proc.h>
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

// Helper to start the first process
void init_starter(void) {
    exec("./bin/sbush");
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

  // An empty page table.
  p->pagetable = uvmcreate();
  if(p->pagetable == 0){
    kfree(p->kstack);
    kfree(p->trapframe);
    p->state = UNUSED;
    return 0;
  }

  // Set up new context to start executing at forkret
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64_t)forkret;
  p->context.sp = (uint64_t)p->kstack + PAGE_SIZE;

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
  const char *name = "sbush";
  for(int i=0; i<6; i++) p->name[i] = name[i];
}

// Give up the CPU for one scheduling round.
void yield(void) {
  if(current_proc) {
    current_proc->state = RUNNABLE;
    swtch(&current_proc->context, &scheduler_context);
  }
}

void scheduler(void) {
  struct proc *p;

  for(;;){
    for(p = proc; p < &proc[64]; p++) {
      if(p->state == RUNNABLE) {
        p->state = RUNNING;
        current_proc = p;
        
        swtch(&scheduler_context, &p->context);

        current_proc = 0;
      }
    }
  }
}
