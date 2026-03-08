# User-Level Threads via CLONE_VM Extension

**Date**: 2026-03-08
**Branch**: scheduler
**Status**: Design (not yet implemented)

## 1. Overview

Extend `sys_clone` to support Linux-style clone flags (`CLONE_VM`, `CLONE_FILES`,
`CLONE_SIGHAND`) enabling user-level threads that share address space, file
descriptors, and signal handlers with their parent. Currently `sys_clone`
(trap.c:308) always performs a full COW fork with separate pagetable, separate
ofile[] copy, and separate sigactions[]. This design adds flag-driven sharing
with reference-counted resources.

---

## 2. Files Changed and Line Ranges

### kernel/include/syscall.h (lines 71-73)
- Add `CLONE_VM`, `CLONE_FILES`, `CLONE_SIGHAND` flag constants alongside
  the existing `SIGCHLD` definition.

### kernel/include/proc.h (lines 82-114)
- Add new fields to `struct proc` for shared-resource bookkeeping:
  refcounted wrapper pointers and clone flags.
- Add new shared-resource structures (defined outside `struct proc`).

### kernel/trap.c (lines 308-368)
- Rewrite `sys_clone()` to parse flags from `a0` and child stack from `a1`.
- Branch on `CLONE_VM`, `CLONE_FILES`, `CLONE_SIGHAND` to share vs. copy.

### kernel/trap.c (lines 1387-1423)
- Modify `SYS_exit` / `SYS_exit_group` path to decrement refcounts on shared
  resources instead of unconditionally freeing them.

### kernel/trap.c (lines 550-571)
- Modify signal default-kill path to also respect shared-resource refcounts.

### kernel/trap.c (lines 1240-1242)
- Modify `SYS_kill` SIGKILL path similarly.

### kernel/exec.c (lines 37-92)
- Handle `CLONE_VM` detach: if the calling thread shares a pagetable, decrement
  the pagetable refcount (do not free old pagetable) before installing the new one.

### kernel/proc.c (lines 73-125)
- Update `allocproc()` to initialize new shared-resource fields to NULL.

### kernel/include/vfs.h
- No changes required. `struct open_file` already has `refcount`.

### kernel/vm.c
- No structural changes. Existing `uvmunmap`, `uvmcreate`, etc. remain usable.
  A new `free_user_pagetable()` helper may be added for clean pagetable teardown.

---

## 3. New Data Structures and Fields

### 3.1 Shared Pagetable Wrapper

```c
/* kernel/include/proc.h -- new structure */
struct shared_pagetable {
    pagetable_t pt;         /* the Sv39 page table root */
    int         refcount;   /* number of procs sharing this pagetable */
};
```

### 3.2 Shared File Table Wrapper

```c
/* kernel/include/proc.h -- new structure */
struct shared_ftable {
    struct open_file *ofile[NOFILE]; /* the fd array */
    int               refcount;     /* number of procs sharing this table */
};
```

### 3.3 Shared Signal Actions Wrapper

```c
/* kernel/include/proc.h -- new structure */
struct shared_sigactions {
    struct kern_sigaction actions[32]; /* one per signal */
    int                  refcount;    /* number of procs sharing */
};
```

### 3.4 New Fields in struct proc

Add the following fields (after the existing `stack_bottom` field, around line 113):

```c
    /* Clone/thread sharing state */
    int clone_flags;                          /* flags this proc was created with */
    struct shared_pagetable  *shared_pt;      /* non-NULL if CLONE_VM */
    struct shared_ftable     *shared_ft;      /* non-NULL if CLONE_FILES */
    struct shared_sigactions *shared_sa;      /* non-NULL if CLONE_SIGHAND */
```

When `clone_flags` has `CLONE_VM` set, the process uses `shared_pt->pt` as its
pagetable (and `proc.pagetable` points to the same value for scheduler
compatibility). Similarly for `CLONE_FILES` and `CLONE_SIGHAND`.

For a non-threaded process (traditional fork or init), these pointers are NULL
and the process owns its pagetable, ofile[], and sigactions[] directly as today.

### 3.5 Flag Constants

```c
/* kernel/include/syscall.h -- add near existing SIGCHLD */
#define CLONE_VM      0x00000100
#define CLONE_FILES   0x00000400
#define CLONE_SIGHAND 0x00000800
```

These match the Linux values.

---

## 4. sys_clone Changes (trap.c:308)

### 4.1 Signature and Flag Parsing

The current `sys_clone(void)` reads no arguments. The new version reads:

- `a0` (tf->a0): clone flags (e.g., `CLONE_VM | CLONE_FILES | SIGCHLD`)
- `a1` (tf->a1): child stack pointer (0 means "use parent's sp")

```
static pid_t sys_clone(void) {
    uint64_t flags    = current_proc->trapframe->a0;
    uint64_t child_sp = current_proc->trapframe->a1;
    ...
}
```

Note: `flags` contains both CLONE_* flags and the low-byte exit signal (typically
`SIGCHLD`). Mask the signal out: `int clone_flags = flags & ~0xFF;`

### 4.2 Common Path (all modes)

Regardless of flags, the child always gets:
- Its own `struct proc` slot via `allocproc()` (own PID, kernel stack, trapframe)
- A copy of the parent's trapframe with `a0 = 0` (child return value)
- Parent pointer set (`p->parent = current_proc`)
- Copy of `cwd`, `name`
- Context set to resume at `forkret`
- `clone_flags` stored in `p->clone_flags`

### 4.3 CLONE_VM Path (shared address space)

Instead of `uvmcreate()` + `copy_pagetable_cow()`:

1. If the parent does not yet have a `shared_pt` (i.e., parent was created by
   fork, not clone), **promote** the parent's pagetable to shared:
   - Allocate a `struct shared_pagetable`
   - Set `shared_pt->pt = current_proc->pagetable`
   - Set `shared_pt->refcount = 1`
   - Set `current_proc->shared_pt = shared_pt`
   - Set `current_proc->clone_flags |= CLONE_VM`

2. Increment `current_proc->shared_pt->refcount`.

3. Set `p->shared_pt = current_proc->shared_pt`.

4. Set `p->pagetable = p->shared_pt->pt` (same physical root page).

5. Share the parent's VMA list pointer. Since VMAs describe regions in a shared
   address space, the child needs access. Two approaches:
   - **Option A (simpler)**: Deep-copy the VMA list via `vma_dup()` as today.
     Both parent and child maintain independent VMA lists describing the same
     address space. This is slightly wasteful but avoids refcounting VMAs.
   - **Option B (correct)**: Wrap the VMA list in a shared structure with
     a refcount, similar to the pagetable. This is necessary if either thread
     can `mmap`/`munmap` and the other must see the change.

   **Recommended**: Option A for the initial implementation. VMA lists are small.
   Document that mmap/munmap in one thread does not update the other's VMA list
   (same limitation as early Linux threads). Option B is a follow-up.

6. Child stack pointer: if `child_sp != 0`, set `p->trapframe->sp = child_sp`.
   Otherwise the child inherits the parent's sp (sharing the same stack -- only
   useful for vfork-like semantics, generally the caller should provide a stack).

7. `p->sz`, `p->stack_top`, `p->stack_bottom`, `p->mmap_base` are all copied
   from the parent (they describe the shared address space).

### 4.4 CLONE_FILES Path (shared file table)

Instead of `copy_ofile(p, current_proc)`:

1. If the parent does not yet have a `shared_ft`, promote:
   - Allocate a `struct shared_ftable`
   - Copy `current_proc->ofile[]` into `shared_ft->ofile[]`
   - Set `shared_ft->refcount = 1`
   - Set `current_proc->shared_ft = shared_ft`
   - Point `current_proc->ofile[i]` at `shared_ft->ofile[i]` entries
     (or, change all fd operations to indirect through `shared_ft` when non-NULL)

2. Increment refcount, assign to child.

3. The child's `ofile[]` array is **not used** -- all fd operations on a
   CLONE_FILES process go through `shared_ft->ofile[]`.

**Alternative simpler design**: Instead of a wrapper struct, have the child's
`ofile[]` pointers simply alias the parent's. But this breaks when one thread
exits and its `struct proc` is reused. The wrapper struct is safer.

**Accessor pattern**: Introduce a helper:
```c
static inline struct open_file **proc_ofile(struct proc *p) {
    return p->shared_ft ? p->shared_ft->ofile : p->ofile;
}
```
All fd operations (`alloc_fd`, `close_ofile`, `copy_ofile`, `SYS_openat`,
`SYS_close`, `SYS_dup3`, `SYS_pipe2`, `SYS_read`, `SYS_write`, etc.) must
use this accessor instead of directly accessing `p->ofile[]`.

### 4.5 CLONE_SIGHAND Path (shared signal handlers)

Instead of inheriting sigactions[] by value:

1. If parent has no `shared_sa`, promote:
   - Allocate a `struct shared_sigactions`
   - Copy `current_proc->sigactions[]` into it
   - Set `shared_sa->refcount = 1`
   - Assign to parent

2. Increment refcount, assign to child.

3. Signal handler registration (`SYS_rt_sigaction`, trap.c) must read/write
   through the shared structure when present.

**Accessor pattern**:
```c
static inline struct kern_sigaction *proc_sigactions(struct proc *p) {
    return p->shared_sa ? p->shared_sa->actions : p->sigactions;
}
```

### 4.6 Non-flag Path (traditional COW fork)

When none of `CLONE_VM`, `CLONE_FILES`, `CLONE_SIGHAND` are set, behavior is
identical to today: COW pagetable copy, `copy_ofile()`, sigactions copied by
value. The `shared_*` pointers remain NULL, `clone_flags = 0`.

---

## 5. Exit Path Changes

### 5.1 SYS_exit / SYS_exit_group (trap.c:1387-1423)

Replace the unconditional cleanup with conditional logic:

```
if (current_proc->shared_pt) {
    /* CLONE_VM: decrement pagetable refcount */
    current_proc->shared_pt->refcount--;
    if (current_proc->shared_pt->refcount == 0) {
        /* Last user: free all user pages and page table structures */
        free_user_pages(current_proc->shared_pt->pt);
        vma_free_all(current_proc);
        kmfree(current_proc->shared_pt, sizeof(struct shared_pagetable));
    }
    current_proc->shared_pt = NULL;
    /* Set pagetable to kernel_pagetable so scheduler doesn't use freed pt */
    current_proc->pagetable = kernel_pagetable;
} else {
    /* Traditional fork: free pagetable and user pages as today */
    /* (Note: current code does not free pagetable on exit -- this is
       an existing bug/TODO. The shared_pt design forces fixing this.) */
    vma_free_all(current_proc);
}
```

Same pattern for `shared_ft`:
```
if (current_proc->shared_ft) {
    current_proc->shared_ft->refcount--;
    if (current_proc->shared_ft->refcount == 0) {
        /* Last user: close all fds in the shared table */
        for (int i = 0; i < NOFILE; i++)
            if (current_proc->shared_ft->ofile[i])
                /* close logic here */;
        kmfree(current_proc->shared_ft, sizeof(struct shared_ftable));
    }
    current_proc->shared_ft = NULL;
} else {
    /* Close fds in proc's own ofile[] as today */
    for (int i = 0; i < NOFILE; i++)
        if (current_proc->ofile[i])
            close_ofile(current_proc, i);
}
```

Same pattern for `shared_sa`:
```
if (current_proc->shared_sa) {
    current_proc->shared_sa->refcount--;
    if (current_proc->shared_sa->refcount == 0)
        kmfree(current_proc->shared_sa, sizeof(struct shared_sigactions));
    current_proc->shared_sa = NULL;
} else {
    /* Nothing to free -- sigactions[] is inline */
}
```

### 5.2 Signal Default-Kill Path (trap.c:556-571)

Must apply the same refcount logic as SYS_exit. Extract the exit cleanup into a
helper function to avoid duplication:

```c
static void cleanup_proc_resources(struct proc *p);
```

This helper is called from:
- `SYS_exit` / `SYS_exit_group` handler
- Signal default-kill path (SIG_DFL for fatal signals)
- `SYS_kill` SIGKILL handler (trap.c:1240-1242)

### 5.3 sys_wait4 (trap.c:373-404)

The reaping path (lines 381-393) currently sets `p->state = UNUSED` and clears
pid/parent. It must also:
- Free `p->kstack` (kalloc'd page)
- Free `p->trapframe` (kalloc'd page)
- These are always per-thread, never shared

Currently the kernel leaks kstack and trapframe memory on wait -- this is a
pre-existing issue that should be fixed as part of this work.

---

## 6. exec() Changes for CLONE_VM Threads (exec.c)

When a CLONE_VM thread calls `exec()`, it must **detach** from the shared
address space before loading the new binary.

### 6.1 Detach from Shared Pagetable

Insert at the top of `exec()` (before line 37 `vma_free_all`):

```
if (current_proc->shared_pt) {
    /* Detach from shared pagetable -- do NOT free it */
    current_proc->shared_pt->refcount--;
    if (current_proc->shared_pt->refcount == 0) {
        /* We were the last user. Free the old pagetable. */
        free_user_pages(current_proc->shared_pt->pt);
        kmfree(current_proc->shared_pt, sizeof(struct shared_pagetable));
    }
    current_proc->shared_pt = NULL;
    current_proc->clone_flags &= ~CLONE_VM;
    /* exec will create a brand new pagetable below */
}
```

The existing `pagetable = uvmcreate()` on line 39 then creates a fresh
independent pagetable for the exec'd process.

### 6.2 Detach from Shared File Table

If `CLONE_FILES` is set, exec should **not** detach from the shared file table
by default (this matches Linux behavior -- exec'd process keeps sharing fds
with the thread group). However, if `CLONE_FILES` detach is desired (to match
`unshare(CLONE_FILES)` semantics), insert similar decrement logic and copy the
current shared_ft contents into the proc's own ofile[].

**Recommended**: Keep sharing through exec, matching Linux semantics.

### 6.3 Detach from Shared Signal Handlers

Per POSIX, exec resets all signal handlers to SIG_DFL. So exec must:

```
if (current_proc->shared_sa) {
    current_proc->shared_sa->refcount--;
    if (current_proc->shared_sa->refcount == 0)
        kmfree(current_proc->shared_sa, sizeof(struct shared_sigactions));
    current_proc->shared_sa = NULL;
    current_proc->clone_flags &= ~CLONE_SIGHAND;
}
/* Reset all sigactions to default (already done implicitly
   since allocproc zeroes them, but exec should explicitly clear) */
memset(current_proc->sigactions, 0, sizeof(current_proc->sigactions));
```

---

## 7. Additional Subsystem Changes

### 7.1 COW Fault Handler (trap.c:634-658)

No changes needed. The COW handler operates on PTE flags in the current
process's pagetable. With CLONE_VM, all threads share the same pagetable and
the same physical pages, so there are no COW pages in a CLONE_VM scenario
(the pagetable was never COW-copied). COW faults only arise from traditional
fork.

### 7.2 Stack Auto-Grow (trap.c:661-680)

With CLONE_VM threads, each thread has its own `stack_top` and `stack_bottom`
in its `struct proc`. A page fault for stack growth uses the faulting thread's
bounds, which is correct. However, the auto-grown pages are mapped into the
shared pagetable -- so all threads can see them. This is fine: each thread's
stack is at a different VA range (the caller provides distinct stack pointers).

### 7.3 mmap / munmap (trap.c SYS_mmap / SYS_munmap)

These operate on `current_proc->pagetable` and `current_proc->vma_list`. With
CLONE_VM, `pagetable` is shared, so mmap/munmap in one thread physically maps
pages visible to all threads. The VMA list, however, is per-proc (under the
simpler Option A design), so the other threads' VMA lists become stale.

This is acceptable for the initial implementation. A follow-up can share the
VMA list.

### 7.4 brk / sbrk (SYS_brk)

Similar to mmap: operates on the shared pagetable. The `sz` field in each
`struct proc` should be kept in sync among CLONE_VM threads. This is a known
limitation; a shared `sz` field (inside `shared_pagetable`) would fix it.

### 7.5 Scheduler (proc.c:155-211)

No changes needed. Each thread is a separate `struct proc` entry with its own
state, so the round-robin scheduler picks them independently. The `satp` write
on line 187 uses `p->pagetable`, which for CLONE_VM threads points to the
shared page table root -- this is correct.

### 7.6 forkret (proc.c:26-41)

No changes needed. `forkret` uses `current_proc->pagetable` for `satp` and
`current_proc->trapframe` for register restore. Both are set correctly for
threads.

---

## 8. Helper Functions to Add

### 8.1 free_user_pages()

Walk a pagetable and free all PTE_U physical pages (using kfree, which respects
refcounts). Then free the page table pages themselves (the three levels of page
table nodes). This is needed because the current code never frees the pagetable
on exit (existing bug).

Location: `kernel/vm.c`

```c
void free_user_pages(pagetable_t pt);
```

### 8.2 cleanup_proc_resources()

Consolidated exit cleanup that handles both shared and non-shared resources.

Location: `kernel/trap.c` (static function)

```c
static void cleanup_proc_resources(struct proc *p) {
    /* Handle shared_pt / pagetable */
    /* Handle shared_ft / ofile[] */
    /* Handle shared_sa / sigactions */
    /* Free VMAs */
}
```

### 8.3 proc_ofile() and proc_sigactions()

Inline accessor functions for fd table and signal actions.

Location: `kernel/include/proc.h`

---

## 9. Memory Allocation for Shared Structures

The `struct shared_pagetable`, `struct shared_ftable`, and `struct shared_sigactions`
are allocated via `kmalloc()` (which exists in the kernel, see includes in proc.c
and vm.c). They are freed via `kmfree()` when the refcount drops to zero.

Sizes:
- `shared_pagetable`: 16 bytes (pointer + int + padding)
- `shared_ftable`: NOFILE * 8 + 4 + padding = ~136 bytes
- `shared_sigactions`: 32 * sizeof(kern_sigaction) + 4 = ~772 bytes

All well within kmalloc capacity.

---

## 10. Edge Cases and Pitfalls

### 10.1 Thread Stack Allocation

The caller (userspace `pthread_create` or equivalent) must allocate a stack
(e.g., via `mmap(MAP_ANONYMOUS)`) and pass its top address as the `child_sp`
argument (a1). If `a1 == 0`, the child inherits the parent's sp, which means
both run on the same stack -- this will corrupt the stack immediately. The
kernel does not prevent this; it is a userspace bug.

**Mitigation**: Document that `a1 == 0` with `CLONE_VM` is unsupported and
will crash. Consider returning -EINVAL if `CLONE_VM` is set and `a1 == 0`.

### 10.2 Concurrent Access (No SMP, But Preemption)

SBUnix is single-core (QEMU virt with one hart), but the timer interrupt
causes preemptive scheduling (trap.c:267-270). This means:

- **Pagetable modification**: If thread A is in the middle of mmap (modifying
  PTEs) and gets preempted, thread B runs and accesses the half-mapped region.
  **Risk**: Moderate. Kernel code between ecall entry and swtch runs with
  interrupts disabled (sstatus.SIE cleared by trap entry), so a single syscall
  is atomic with respect to preemption. The risk exists only if a syscall
  voluntarily calls `swtch()` (sleeps) mid-operation.

- **File table modification**: Similar analysis. Fd operations complete within
  a single non-sleeping syscall, so they are safe.

- **Signal handler modification**: `rt_sigaction` is a fast non-sleeping
  syscall, so it is safe.

**Future concern**: If SMP support is added, all shared structures need
spinlocks or similar synchronization.

### 10.3 Thread Group Semantics

Linux groups threads into a "thread group" sharing a TGID (the PID visible to
`getpid()`). Each thread has a unique TID. SBUnix does not need full thread
group semantics for the initial implementation:

- Each thread has its own `pid` (returned by `getpid()`).
- `wait4` only reaps direct children (by `parent` pointer), which works for
  threads since they share the same parent.
- Thread-to-thread `kill()` works since it targets by PID.

This differs from Linux but is acceptable for a teaching OS.

### 10.4 Exit of Parent Before Threads

If the original parent (the process that created threads via CLONE_VM) exits
before its child threads:

- The shared_pt refcount prevents the pagetable from being freed.
- The child threads become orphans (parent is set to NULL or re-parented to
  init). Current wait4 already handles `p->parent == NULL` gracefully (the
  zombie is never reaped, leaking the proc slot).

**Recommended fix**: Re-parent orphaned threads to init (pid 1), same as
current fork semantics. Add re-parenting logic in the exit path:
```c
for (struct proc *p = proc; p < &proc[64]; p++) {
    if (p->parent == current_proc && p->state != UNUSED)
        p->parent = &proc[0]; /* init */
}
```

### 10.5 exec() in a Thread While Others Run

When a CLONE_VM thread calls exec():
- It detaches from the shared pagetable (section 6.1).
- Other threads continue using the shared pagetable.
- The exec'd thread gets a completely new address space.
- The exec'd thread is no longer a "thread" of the original group.

This is correct but surprising. Linux handles this by killing all other threads
in the thread group on exec. SBUnix should document this difference. Killing
siblings on exec is a possible follow-up.

### 10.6 CLONE_SIGHAND Without CLONE_VM

Linux requires `CLONE_SIGHAND` to imply `CLONE_VM` (they must be used together).
SBUnix should enforce this:
```c
if ((clone_flags & CLONE_SIGHAND) && !(clone_flags & CLONE_VM))
    return -EINVAL;
```

Similarly, `CLONE_FILES` can be used independently of `CLONE_VM` (Linux allows
this).

### 10.7 Signal Delivery to Thread Group

With `CLONE_SIGHAND`, all threads sharing the signal actions see the same
handler registrations. However, `sigpending` is per-thread (stored in
`struct proc`). When a signal is sent to a PID:

- If sent to a specific thread's PID, only that thread handles it.
- There is no "process-wide" signal delivery in the initial implementation.

**Follow-up**: Add a `tgid` field and route process-directed signals to any
runnable thread in the group.

### 10.8 TLB Consistency

With CLONE_VM, all threads share one pagetable. When one thread modifies a PTE
(e.g., COW fault resolution, mmap), it issues `sfence.vma`. On a single-hart
system, this is sufficient because only one thread runs at a time and the TLB
is flushed on context switch (satp write). On SMP, IPIs would be needed.

### 10.9 kstack and trapframe Lifetime

Each thread's `kstack` and `trapframe` are allocated by `allocproc()` and are
**never shared**. They must be freed when the thread is reaped (in `sys_wait4`
or when the zombie proc slot is reclaimed). Currently these are leaked -- fix
this as part of the implementation.

---

## 11. Testing Approach

### 11.1 Basic Thread Creation

Write a user program `bin/threadtest/threadtest.c`:

1. `mmap` a 4KB stack for the child thread.
2. Call `clone(CLONE_VM | CLONE_FILES | CLONE_SIGHAND | SIGCHLD, child_stack_top)`.
3. Child: write to a shared global variable, then `exit(42)`.
4. Parent: `wait4` for child, then read the shared global variable.
5. Verify the global was modified (proves shared address space).
6. Verify wait4 returns the child's exit status.

### 11.2 Shared File Descriptors

1. Parent opens a file via `openat`.
2. Clone with `CLONE_FILES`.
3. Child closes the fd.
4. Parent tries to use the fd -- should fail (fd was closed in shared table).

### 11.3 Independent File Descriptors (no CLONE_FILES)

1. Parent opens a file.
2. Clone without `CLONE_FILES` (traditional fork behavior).
3. Child closes the fd.
4. Parent uses the fd -- should succeed (separate fd table).

### 11.4 exec() in a Thread

1. Create a CLONE_VM thread.
2. Child calls `execve("/bin/echo", ...)`.
3. Verify parent continues running (shared pagetable not destroyed).
4. Verify child runs the new binary successfully.

### 11.5 Thread Exit Without Destroying Shared Resources

1. Create two CLONE_VM threads from the same parent.
2. First child exits.
3. Second child and parent continue -- verify they can still access shared memory.
4. Second child exits.
5. Parent exits -- pagetable freed (refcount reaches 0).

### 11.6 Stress Test

1. Create multiple threads (up to the 64-proc table limit).
2. Each thread increments a shared counter N times.
3. Verify final counter value (on single-core, no races, so it should be exact).

### 11.7 Signal Handler Sharing

1. Create thread with `CLONE_SIGHAND`.
2. Parent registers a handler for SIGUSR1 (signal 10) via `rt_sigaction`.
3. Send SIGUSR1 to the child thread.
4. Verify the handler (registered by parent) runs in the child.

### 11.8 Edge Case: CLONE_VM with child_sp == 0

1. Call `clone(CLONE_VM, 0)`.
2. Expected: kernel returns -EINVAL (if we add the guard) or both threads
   corrupt the stack and crash (if no guard -- useful for demonstrating the
   pitfall to students).

---

## 12. Implementation Order

1. **Add flag constants** to syscall.h.
2. **Add shared structures and fields** to proc.h.
3. **Initialize new fields** in `allocproc()` (proc.c).
4. **Add accessor helpers** (`proc_ofile()`, `proc_sigactions()`) to proc.h.
5. **Refactor exit cleanup** into `cleanup_proc_resources()` in trap.c.
6. **Implement flag parsing** in `sys_clone()`.
7. **Implement CLONE_VM** path (shared pagetable with refcount).
8. **Implement CLONE_FILES** path (shared fd table with refcount).
9. **Implement CLONE_SIGHAND** path (shared signal handlers with refcount).
10. **Update exec()** to detach from shared pagetable and signal handlers.
11. **Update all fd-accessing syscalls** to use `proc_ofile()` accessor.
12. **Update signal syscalls** to use `proc_sigactions()` accessor.
13. **Add `free_user_pages()`** to vm.c and fix the pagetable leak on exit.
14. **Fix kstack/trapframe leak** in wait4 reap path.
15. **Write test programs** and verify under `make qemu`.

---

## 13. Summary of All Touched Files

| File | Nature of Change |
|------|-----------------|
| `kernel/include/syscall.h` | Add CLONE_VM, CLONE_FILES, CLONE_SIGHAND constants |
| `kernel/include/proc.h` | Add shared_pagetable, shared_ftable, shared_sigactions structs; add fields to struct proc; add accessor inlines |
| `kernel/proc.c` | Initialize new fields in allocproc() |
| `kernel/trap.c` | Rewrite sys_clone(); refactor exit paths; update fd/signal accessors throughout (~20 callsites) |
| `kernel/exec.c` | Add CLONE_VM/CLONE_SIGHAND detach logic at top of exec() |
| `kernel/vm.c` | Add free_user_pages() helper |
