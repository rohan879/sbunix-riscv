# Kernel Architecture Gaps Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement four major kernel subsystems: signal delivery, kmalloc (sub-page allocator), COW fork, and page fault recovery (stack auto-grow + demand paging).

**Architecture:** All changes in `kernel/` source files. Builds on the existing Sv39 page table infrastructure (`vm.c`), process model (`proc.c`), and trap handling (`trap.c`).

**Tech Stack:** C (freestanding kernel), RISC-V Sv39, `kernel/include/` headers.

**Build/Test:** `cd sbunix-skeleton && source tools/sourceme.sh && make clean && make && make qemu`

**Ordering:** Signal delivery first (most test impact), then kmalloc, then COW fork, then page fault recovery.

---

## Part A: Signal Delivery

Signal delivery requires diverting the trap return path to execute a user-space signal handler before returning to the interrupted code. The kernel already stores `sigactions[]` per process (from Workstream 2). This plan adds the actual delivery mechanism.

### Task A1: Add signal trampoline page

**Files:**
- Create: `kernel/sigtramp.S` (signal return trampoline in assembly)
- Modify: `kernel/include/proc.h` (add `sig_trampoline` VA constant)
- Modify: `kernel/vm.c` (map trampoline into every user process)

The trampoline is a small code snippet mapped at a fixed user VA that calls `rt_sigreturn` to restore the original context after the handler runs.

**Step 1: Write the trampoline**

Create `kernel/sigtramp.S`:

```asm
.section .text
.globl sigtrampoline
.globl sigtrampoline_end
sigtrampoline:
    /* a7 = SYS_rt_sigreturn (139) */
    li a7, 139
    ecall
sigtrampoline_end:
```

**Step 2: Add constants to `proc.h`**

```c
/* Signal trampoline is mapped at this user VA in every process */
#define SIG_TRAMPOLINE_VA  0x3FFFFFF000UL  /* just below top of Sv39 user space */
#define SYS_rt_sigreturn   139
```

**Step 3: Map trampoline in `uvmcreate()` or wherever user page tables are built**

In `kernel/vm.c`, after creating a user page table, map the trampoline:

```c
extern char sigtrampoline[];
extern char sigtrampoline_end[];

/* In uvmcreate() or a new helper called from allocproc/exec: */
void uvmmap_sigtrampoline(pagetable_t pagetable) {
    /* Allocate a page, copy trampoline code, map as user-read-execute */
    char *pg = kalloc();
    memset(pg, 0, PAGE_SIZE);
    int sz = sigtrampoline_end - sigtrampoline;
    memcpy(pg, sigtrampoline, sz);
    mappages(pagetable, SIG_TRAMPOLINE_VA, PAGE_SIZE, (uint64_t)pg,
             PTE_R | PTE_X | PTE_U);
}
```

Call `uvmmap_sigtrampoline()` from `allocproc()` (after creating the user page table) and from the SYS_execve handler (after creating the new page table).

**Step 4: Build and verify**

```bash
make clean && make
```

**Step 5: Commit**

```bash
git add kernel/sigtramp.S kernel/include/proc.h kernel/vm.c kernel/proc.c
git commit -m "ENH: Added signal trampoline page mapped into user processes"
```

---

### Task A2: Add `SYS_rt_sigreturn` handler

**Files:**
- Modify: `kernel/trap.c`
- Modify: `kernel/include/syscall.h` (add `SYS_rt_sigreturn 139`)

When the signal handler finishes, the trampoline calls `SYS_rt_sigreturn`. This restores the original trapframe that was saved before signal delivery.

**Step 1: Add syscall number**

In `kernel/include/syscall.h`:

```c
#define SYS_rt_sigreturn  139
```

**Step 2: Add handler in `kernel/trap.c`**

```c
else if (num == 139) { /* SYS_rt_sigreturn */
    /* The saved trapframe is on the user stack.
     * When we delivered the signal, we pushed the original trapframe
     * onto the user stack. Now we restore it. */
    uint64_t sp = tf->sp; /* points to saved trapframe on user stack */

    /* Read saved trapframe from user stack */
    struct trapframe saved;
    /* Copy from user memory (page-walk each field) */
    for (uint64_t off = 0; off < sizeof(struct trapframe); off += 8) {
        pte_t *pte = walk(current_proc->pagetable, sp + off, 0);
        if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
            uint64_t pa = PTE2PA(*pte) + ((sp + off) & 0xFFF);
            *(uint64_t *)((char *)&saved + off) = *(uint64_t *)pa;
        }
    }

    /* Restore the original trapframe */
    memcpy(tf, &saved, sizeof(struct trapframe));
    /* Don't modify tf->a0 — it was already set in the saved frame */
    return; /* Skip the normal tf->a0 = retval assignment */
}
```

**Step 3: Build, verify, commit**

```bash
make clean && make
git add kernel/include/syscall.h kernel/trap.c
git commit -m "ENH: Added SYS_rt_sigreturn handler for signal context restore"
```

---

### Task A3: Implement signal delivery on trap return

**Files:**
- Modify: `kernel/trap.c` (check for pending signals before returning to user mode)

The core mechanism: before returning to user mode (at the end of the trap handler), check if the current process has pending unblocked signals. If so, save the current trapframe on the user stack, set up the trapframe to jump to the signal handler, and return to user mode at the handler address.

**Step 1: Add `deliver_signal()` helper**

In `kernel/trap.c`:

```c
static void deliver_signal(struct trapframe *tf, struct proc *p) {
    /* Find highest-priority pending unblocked signal */
    uint64_t deliverable = p->sigpending & ~p->sigmask;
    if (deliverable == 0)
        return;

    int signum = 0;
    for (int i = 1; i < 32; i++) {
        if (deliverable & (1UL << i)) {
            signum = i;
            break;
        }
    }

    /* Clear from pending */
    p->sigpending &= ~(1UL << signum);

    uint64_t handler = p->sigactions[signum].handler;

    /* SIG_DFL (0) — default action */
    if (handler == 0) {
        /* Default: terminate process for most signals */
        if (signum == SIGCHLD || signum == SIGURG || signum == SIGWINCH)
            return; /* ignore by default */
        p->exit_status = 128 + signum;
        p->state = ZOMBIE;
        if (p->parent && p->parent->state == SLEEPING)
            p->parent->state = RUNNABLE;
        swtch(&p->context, &scheduler_context);
        return;
    }

    /* SIG_IGN (1) — ignore */
    if (handler == 1)
        return;

    /* User handler: save trapframe on user stack, redirect execution */
    uint64_t sp = tf->sp;

    /* Push original trapframe onto user stack */
    sp -= sizeof(struct trapframe);
    sp &= ~15UL; /* 16-byte align */

    /* Copy trapframe to user stack */
    for (uint64_t off = 0; off < sizeof(struct trapframe); off += 8) {
        pte_t *pte = walk(p->pagetable, sp + off, 0);
        if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
            uint64_t pa = PTE2PA(*pte) + ((sp + off) & 0xFFF);
            *(uint64_t *)pa = *(uint64_t *)((char *)tf + off);
        }
    }

    /* Set up trapframe to call handler(signum) with return to trampoline */
    tf->epc = handler;          /* jump to signal handler */
    tf->a0 = (uint64_t)signum;  /* first arg: signal number */
    tf->sp = sp;                /* stack points to saved trapframe */
    tf->ra = SIG_TRAMPOLINE_VA; /* return address → trampoline → sigreturn */

    /* Block this signal during handler execution */
    p->sigmask |= p->sigactions[signum].mask;
    if (!(p->sigactions[signum].flags & 0x40000000)) /* SA_NODEFER */
        p->sigmask |= (1UL << signum);
}
```

**Step 2: Call `deliver_signal()` before returning to user mode**

At the end of the trap handler, just before calling `trap_restore` (or wherever control returns to user mode), add:

```c
/* Deliver pending signals before returning to user mode */
if (current_proc && current_proc->sigpending & ~current_proc->sigmask) {
    deliver_signal(tf, current_proc);
}
```

**Step 3: Update `SYS_kill` to set pending bit**

Modify the existing SYS_kill handler to set the pending bit instead of (or in addition to) immediately killing:

```c
else if (num == 129) { /* SYS_kill */
    pid_t target_pid = (pid_t)tf->a0;
    int sig = (int)tf->a1;
    struct proc *target = NULL;
    for (int i = 0; i < NPROC; i++) {
        if (proc[i].state != UNUSED && proc[i].pid == target_pid) {
            target = &proc[i];
            break;
        }
    }
    if (!target) {
        tf->a0 = (uint64_t)-3; /* -ESRCH */
    } else {
        if (sig == 9) { /* SIGKILL — always kill immediately */
            target->exit_status = 128 + 9;
            target->state = ZOMBIE;
            if (target->parent && target->parent->state == SLEEPING)
                target->parent->state = RUNNABLE;
        } else if (sig > 0 && sig < 32) {
            target->sigpending |= (1UL << sig);
            /* Wake target if sleeping so it can handle the signal */
            if (target->state == SLEEPING)
                target->state = RUNNABLE;
        }
        tf->a0 = 0;
    }
}
```

**Step 4: Build, test, commit**

```bash
make clean && make && make qemu
# Test: signal(SIGALRM, handler); alarm(1); → handler should fire
git add kernel/trap.c
git commit -m "ENH: Implemented signal delivery to userspace handlers"
```

---

## Part B: kmalloc (Sub-Page Allocator)

### Task B1: Implement slab-style kmalloc

**Files:**
- Create: `kernel/kmalloc.c`
- Create: `kernel/include/kmalloc.h`

A simple power-of-two allocator: maintains free lists for sizes 16, 32, 64, 128, 256, 512, 1024, 2048. Each slab is carved from a `kalloc()` page.

**Step 1: Create `kernel/include/kmalloc.h`**

```c
#ifndef _KMALLOC_H
#define _KMALLOC_H

#include <riscv.h>

void *kmalloc(uint64_t size);
void  kmfree(void *ptr, uint64_t size);

#endif
```

**Step 2: Create `kernel/kmalloc.c`**

```c
#include <kmalloc.h>
#include <kalloc.h>
#include <string.h>

#define NBUCKETS 8
/* bucket sizes: 16, 32, 64, 128, 256, 512, 1024, 2048 */
static int bucket_size[NBUCKETS] = {16, 32, 64, 128, 256, 512, 1024, 2048};

struct kmalloc_node {
    struct kmalloc_node *next;
};

static struct kmalloc_node *freelists[NBUCKETS];

static int size_to_bucket(uint64_t size) {
    for (int i = 0; i < NBUCKETS; i++)
        if (size <= (uint64_t)bucket_size[i])
            return i;
    return -1; /* too large — use kalloc directly */
}

void *kmalloc(uint64_t size) {
    if (size == 0)
        return 0;

    int b = size_to_bucket(size);
    if (b < 0) {
        /* Large allocation: round up to pages */
        uint64_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        /* For simplicity, only support single-page large allocs */
        if (npages > 1)
            return 0;
        return kalloc();
    }

    /* Check free list */
    if (freelists[b]) {
        struct kmalloc_node *node = freelists[b];
        freelists[b] = node->next;
        return (void *)node;
    }

    /* Allocate a new page and carve it into chunks */
    char *page = kalloc();
    if (!page)
        return 0;
    memset(page, 0, PAGE_SIZE);

    int chunk_size = bucket_size[b];
    int nchunks = PAGE_SIZE / chunk_size;

    /* First chunk is returned to caller */
    /* Remaining chunks go on free list */
    for (int i = 1; i < nchunks; i++) {
        struct kmalloc_node *node = (struct kmalloc_node *)(page + i * chunk_size);
        node->next = freelists[b];
        freelists[b] = node;
    }

    return (void *)page;
}

void kmfree(void *ptr, uint64_t size) {
    if (!ptr)
        return;

    int b = size_to_bucket(size);
    if (b < 0) {
        kfree(ptr);
        return;
    }

    struct kmalloc_node *node = (struct kmalloc_node *)ptr;
    node->next = freelists[b];
    freelists[b] = node;
}
```

**Step 3: Build and verify**

```bash
make clean && make
```

**Step 4: Commit**

```bash
git add kernel/kmalloc.c kernel/include/kmalloc.h
git commit -m "ENH: Added kmalloc sub-page allocator with power-of-two buckets"
```

---

## Part C: COW Fork

### Task C1: Add page reference counting

**Files:**
- Modify: `kernel/kalloc.c` (add refcount array)
- Modify: `kernel/include/kalloc.h` (export refcount functions)

**Step 1: Add refcount infrastructure**

In `kernel/kalloc.c`, add:

```c
/* Reference counts for physical pages */
#define MAX_PAGES ((128 * 1024 * 1024) / PAGE_SIZE) /* 128MB / 4KB = 32768 */
static int page_refcount[MAX_PAGES];

static int pa_to_index(uint64_t pa) {
    return (pa - 0x80000000UL) / PAGE_SIZE;
}

void page_ref_inc(uint64_t pa) {
    int idx = pa_to_index(pa);
    if (idx >= 0 && idx < MAX_PAGES)
        page_refcount[idx]++;
}

void page_ref_dec(uint64_t pa) {
    int idx = pa_to_index(pa);
    if (idx >= 0 && idx < MAX_PAGES)
        page_refcount[idx]--;
}

int page_ref_get(uint64_t pa) {
    int idx = pa_to_index(pa);
    if (idx >= 0 && idx < MAX_PAGES)
        return page_refcount[idx];
    return 0;
}
```

Modify `kalloc()` to set refcount to 1:

```c
void *kalloc(void) {
    struct run *r = kmem.freelist;
    if (r) {
        kmem.freelist = r->next;
        page_refcount[pa_to_index((uint64_t)r)] = 1;
    }
    return (void *)r;
}
```

Modify `kfree()` to only free when refcount reaches 0:

```c
void kfree(void *pa) {
    int idx = pa_to_index((uint64_t)pa);
    if (idx >= 0 && idx < MAX_PAGES) {
        page_refcount[idx]--;
        if (page_refcount[idx] > 0)
            return; /* still referenced */
    }
    /* ... existing free logic ... */
}
```

**Step 2: Export in `kernel/include/kalloc.h`**

```c
void page_ref_inc(uint64_t pa);
void page_ref_dec(uint64_t pa);
int  page_ref_get(uint64_t pa);
```

**Step 3: Build, verify, commit**

```bash
make clean && make
git add kernel/kalloc.c kernel/include/kalloc.h
git commit -m "ENH: Added page reference counting to physical allocator"
```

---

### Task C2: Implement COW fork

**Files:**
- Modify: `kernel/trap.c` (`copy_pagetable` → COW version)
- Modify: `kernel/trap.c` (page fault handler for COW)
- Modify: `kernel/include/riscv.h` (add PTE_COW flag)

**Step 1: Define COW PTE bit**

In `kernel/include/riscv.h`, add:

```c
#define PTE_COW (1L << 8)  /* RSW bit 0: copy-on-write marker */
```

The RISC-V PTE has two "reserved for software" (RSW) bits at positions 8-9. We use bit 8 for COW.

**Step 2: Replace `copy_pagetable` with COW version**

In `kernel/trap.c`, replace the existing `copy_pagetable` function:

```c
static int copy_pagetable_cow(pagetable_t dst, pagetable_t src, uint64_t sz) {
    for (uint64_t va = 0; va < sz; va += PAGE_SIZE) {
        pte_t *pte = walk(src, va, 0);
        if (!pte || !(*pte & PTE_V))
            continue;
        if (!(*pte & PTE_U))
            continue;

        uint64_t pa = PTE2PA(*pte);
        int flags = PTE_FLAGS(*pte);

        if (flags & PTE_W) {
            /* Writable page: mark read-only + COW in both parent and child */
            *pte = PA2PTE(pa) | ((flags & ~PTE_W) | PTE_COW | PTE_V);
            flags = (flags & ~PTE_W) | PTE_COW;
        }

        /* Share the same physical page */
        page_ref_inc(pa);
        if (mappages(dst, va, PAGE_SIZE, pa, flags) < 0)
            return -1;
    }
    return 0;
}
```

Update `sys_clone()` to call `copy_pagetable_cow` instead of `copy_pagetable`.

**Step 3: Handle COW page faults**

In the page fault handler (scause 15 = store page fault), add COW resolution:

```c
if (cause == SCAUSE_STORE_PAGE_FAULT) {
    uint64_t va = PGROUNDDOWN(stval);
    pte_t *pte = walk(current_proc->pagetable, va, 0);

    if (pte && (*pte & PTE_V) && (*pte & PTE_COW)) {
        /* COW fault: copy the page and make it writable */
        uint64_t pa = PTE2PA(*pte);
        int flags = PTE_FLAGS(*pte);

        if (page_ref_get(pa) == 1) {
            /* Only reference: just make it writable again */
            *pte = PA2PTE(pa) | ((flags & ~PTE_COW) | PTE_W);
        } else {
            /* Multiple references: allocate new page and copy */
            char *newpage = kalloc();
            if (!newpage) {
                /* Out of memory — kill process */
                current_proc->exit_status = 139;
                current_proc->state = ZOMBIE;
                if (current_proc->parent && current_proc->parent->state == SLEEPING)
                    current_proc->parent->state = RUNNABLE;
                swtch(&current_proc->context, &scheduler_context);
                return;
            }
            memcpy(newpage, (void *)pa, PAGE_SIZE);
            *pte = PA2PTE((uint64_t)newpage) | ((flags & ~PTE_COW) | PTE_W);
            page_ref_dec(pa); /* old page loses a reference */
        }
        /* Flush TLB for this VA */
        asm volatile("sfence.vma %0, zero" : : "r"(va));
        return; /* resume execution — the store will now succeed */
    }

    /* Not a COW fault — fall through to existing fatal handler */
}
```

**Step 4: Build, test, commit**

```bash
make clean && make && make qemu
# Test: fork a child that writes to a variable — parent should see original value
git add kernel/include/riscv.h kernel/trap.c
git commit -m "ENH: Implemented COW fork with lazy page copying on write fault"
```

---

## Part D: Page Fault Recovery

### Task D1: Stack auto-grow

**Files:**
- Modify: `kernel/trap.c` (page fault handler)
- Modify: `kernel/include/proc.h` (add stack bounds to struct proc)

**Step 1: Track stack bounds in `struct proc`**

Add to `struct proc`:

```c
  uint64_t stack_top;     /* top of user stack (highest VA) */
  uint64_t stack_bottom;  /* lowest allocated stack page */
```

Initialize in `allocproc()` and the SYS_execve handler. The current exec allocates 8 pages of stack, so:

```c
p->stack_top = USERTOP;              /* e.g., 0x80000000 or wherever stack starts */
p->stack_bottom = USERTOP - 8 * PAGE_SIZE;
```

(Adjust `USERTOP` based on the actual stack VA used in the exec handler.)

**Step 2: Add stack auto-grow to page fault handler**

In the page fault handler, before the fatal kill:

```c
/* Check if fault is near the stack — auto-grow */
if (stval >= current_proc->stack_bottom - 16 * PAGE_SIZE &&
    stval < current_proc->stack_top) {
    uint64_t va = PGROUNDDOWN(stval);
    /* Grow stack down */
    while (current_proc->stack_bottom > va) {
        current_proc->stack_bottom -= PAGE_SIZE;
        char *pg = kalloc();
        if (!pg) break;
        memset(pg, 0, PAGE_SIZE);
        mappages(current_proc->pagetable, current_proc->stack_bottom,
                 PAGE_SIZE, (uint64_t)pg, PTE_R | PTE_W | PTE_U);
    }
    if (current_proc->stack_bottom <= va) {
        asm volatile("sfence.vma %0, zero" : : "r"(va));
        return; /* resume — page is now mapped */
    }
}
```

**Step 3: Build, test, commit**

```bash
make clean && make && make qemu
# Test: deep recursion should not crash immediately
git add kernel/include/proc.h kernel/proc.c kernel/trap.c
git commit -m "ENH: Added stack auto-grow on page fault"
```

---

### Task D2: Demand paging for mmap regions

**Files:**
- Modify: `kernel/trap.c` (page fault handler + mmap handler)

Currently `SYS_mmap` eagerly allocates pages. Change it to record the mapping and allocate on first access.

**Step 1: Make mmap lazy**

In the SYS_mmap handler, instead of calling `uvmalloc()`, just record the region:

```c
/* Record region but don't allocate pages yet */
current_proc->mmap_regions[slot].va = va;
current_proc->mmap_regions[slot].len = length;
/* Don't call uvmalloc — pages will be allocated on demand */
tf->a0 = va;
```

**Step 2: Handle mmap page faults**

In the page fault handler, check if the faulting address falls within an mmap region:

```c
/* Check mmap regions for demand paging */
for (int i = 0; i < NMMAP; i++) {
    if (current_proc->mmap_regions[i].len == 0) continue;
    uint64_t rva = current_proc->mmap_regions[i].va;
    uint64_t rlen = current_proc->mmap_regions[i].len;
    if (stval >= rva && stval < rva + rlen) {
        uint64_t va = PGROUNDDOWN(stval);
        char *pg = kalloc();
        if (!pg) break;
        memset(pg, 0, PAGE_SIZE);
        mappages(current_proc->pagetable, va, PAGE_SIZE, (uint64_t)pg,
                 PTE_R | PTE_W | PTE_U);
        asm volatile("sfence.vma %0, zero" : : "r"(va));
        return; /* resume */
    }
}
```

**Step 3: Build, test, commit**

```bash
make clean && make && make qemu
git add kernel/trap.c
git commit -m "ENH: Added demand paging for anonymous mmap regions"
```

---

## Part E: VFS Abstraction (Foundation)

### Task E1: Define VFS operations struct

**Files:**
- Create: `kernel/include/vfs.h`
- Modify: `kernel/include/proc.h` (update `struct file` to use VFS ops)

**Step 1: Create `kernel/include/vfs.h`**

```c
#ifndef _VFS_H
#define _VFS_H

#include <riscv.h>

/* Forward declarations */
struct file;
struct stat;

/* File operations table */
struct file_ops {
    ssize_t (*read)(struct file *f, char *buf, size_t n);
    ssize_t (*write)(struct file *f, const char *buf, size_t n);
    int     (*close)(struct file *f);
    int     (*stat)(struct file *f, struct stat *st);
    off_t   (*lseek)(struct file *f, off_t offset, int whence);
};

/* Updated file struct with VFS ops */
struct vfile {
    int used;
    int type;              /* VFILE_NONE, VFILE_PIPE, VFILE_TAR, VFILE_CONSOLE */
    struct file_ops *ops;  /* operation dispatch */
    /* Type-specific data */
    union {
        struct {
            int pipe_id;
            int pipe_read;
            int pipe_write;
        } pipe;
        struct {
            uint64_t offset;
            uint64_t size;
            char *data;
        } tar;
        struct {
            int fd_num; /* 0=stdin, 1=stdout, 2=stderr */
        } console;
    };
};

#define VFILE_NONE    0
#define VFILE_PIPE    1
#define VFILE_TAR     2
#define VFILE_CONSOLE 3

#endif /* _VFS_H */
```

**NOTE:** Migrating the existing `struct file` in `proc.h` to `struct vfile` is a large refactor. This task defines the interface. Actual migration of the syscall handlers to use VFS dispatch would be a follow-up task that touches every file operation in trap.c. Only pursue this if there's time — the current direct-dispatch approach works fine for grading.

**Step 2: Build, verify, commit**

```bash
make clean && make
git add kernel/include/vfs.h
git commit -m "ENH: Added VFS file operations interface definition"
```
