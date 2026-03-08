# On-Disk Swap and Demand Paging via virtio-blk

Design document for adding swap space and demand paging to SBUnix.

**Date**: 2026-03-08
**Branch**: scheduler
**Status**: Design (not yet implemented)

---

## 1. Swap Space Layout on Disk

### Current disk usage

The tarfs filesystem is embedded directly in the kernel ELF (the `.tarfs`
linker section), so the virtio-blk disk is entirely unused by the filesystem.
The only current user is `virtio_blk_test()`, which writes to sectors 0-799
during initialization testing. Swap can therefore occupy the entire disk, or a
well-defined region of it.

### Proposed layout

Reserve the **last N 4 KB blocks** of the disk as swap space. One 4 KB swap
slot corresponds to one page, and each 4 KB block is 8 disk sectors (512 bytes
each).

```
Disk layout (capacity reported by device in 512-byte sectors):

  Sector 0                                    Sector (capacity - 1)
  |---------- filesystem / free ----------|-------- swap region --------|
                                          ^
                                   SWAP_START_SECTOR
```

Constants (to be defined in a new `kernel/include/swap.h`):

```c
#define SWAP_NSLOTS       1024              /* 1024 pages = 4 MB of swap  */
#define SECTORS_PER_PAGE  (PAGE_SIZE / 512) /* 8 sectors per 4 KB page    */
#define SWAP_NSECTORS     (SWAP_NSLOTS * SECTORS_PER_PAGE) /* 8192 sectors */
```

At init time, compute:

```c
/* blkdev.capacity is in 512-byte sectors */
uint64_t SWAP_START_SECTOR = blkdev.capacity - SWAP_NSECTORS;
```

A swap slot `i` (0 <= i < SWAP_NSLOTS) maps to disk sector:

```c
uint64_t sector = SWAP_START_SECTOR + (uint64_t)i * SECTORS_PER_PAGE;
```

With 1024 slots, this provides 4 MB of swap space -- adequate for a 128 MB
teaching OS with 64 processes.

### Initialization

`swap_init()` is called after `virtio_blk_init()` during boot. It verifies
that the disk capacity is large enough (at least `SWAP_NSECTORS` sectors),
computes `SWAP_START_SECTOR`, and zeroes the swap bitmap.

---

## 2. Swap Bitmap Data Structure

A simple bitmap tracks which swap slots are free (0) or occupied (1).

```c
/* In kernel/swap.c */
#define SWAP_BITMAP_WORDS  ((SWAP_NSLOTS + 63) / 64)
static uint64_t swap_bitmap[SWAP_BITMAP_WORDS];
static int swap_used_count;   /* number of slots currently in use */
```

### Operations

```c
/* Allocate a free swap slot. Returns slot index [0, SWAP_NSLOTS),
 * or -1 if swap is full. Linear scan from slot 0. */
int swap_alloc(void);

/* Free a swap slot, marking it available for reuse. */
void swap_free(int slot);

/* Check whether a slot is occupied. */
int swap_is_used(int slot);
```

`swap_alloc()` scans the bitmap words for a word that is not all-ones, then
uses `__builtin_ctzl()` (count trailing zeros) to find the first free bit
within that word. This is O(SWAP_NSLOTS/64) in the worst case but fast in
practice.

No locking is needed because SBUnix is single-hart (single CPU) and swap I/O
is synchronous (interrupts are disabled during kernel-mode trap handling).

---

## 3. PTE Encoding for Swapped-Out Pages

### RISC-V Sv39 PTE bit layout (reminder)

```
Bits  63-54: Reserved (zero)
Bits  53-10: PPN[2:0] (physical page number, 44 bits)
Bits   9-8:  RSW (software-defined, 2 bits)
Bit      7:  D (dirty)
Bit      6:  A (accessed)
Bit      5:  G (global)
Bit      4:  U (user)
Bit      3:  X (execute)
Bit      2:  W (write)
Bit      1:  R (read)
Bit      0:  V (valid)
```

When a page is swapped out, **PTE_V is cleared** so any access faults. The
hardware ignores all other bits when V=0, so we can repurpose the remaining
bits to store swap metadata.

### Encoding

```
Swapped-out PTE:
  Bit  0:     0          (PTE_V cleared -- triggers page fault on access)
  Bit  8:     1          (PTE_COW position, repurposed as PTE_SWAP marker)
  Bits 9:     <original COW flag preserved>
  Bits 10-53: swap slot index (44 bits, only 10 needed for 1024 slots)
  Bits 1-7:   original permission flags (R, W, X, U, G, A, D) preserved
```

Define a new marker in `riscv.h`:

```c
#define PTE_SWAP    (1L << 8)   /* page is swapped out (V must be 0) */
```

Note: `PTE_SWAP` reuses the same bit position as `PTE_COW`. This is safe
because `PTE_COW` is only meaningful when `PTE_V=1` (page is present), while
`PTE_SWAP` is only meaningful when `PTE_V=0` (page is absent). The two states
are mutually exclusive.

Helper macros:

```c
/* Encode a swapped-out PTE. Store original flags (excluding V) and slot. */
#define PTE_SWAP_ENCODE(slot, orig_flags) \
    (((uint64_t)(slot) << 10) | PTE_SWAP | ((orig_flags) & 0xFE))
    /* 0xFE = bits 1-7 (R,W,X,U,G,A,D), excludes V in bit 0 */

/* Decode swap slot from a swapped-out PTE */
#define PTE_SWAP_SLOT(pte)  ((int)((pte) >> 10))

/* Recover the original permission flags for remapping on swap-in */
#define PTE_SWAP_FLAGS(pte) ((int)((pte) & 0xFE))

/* Test whether a PTE represents a swapped-out page */
#define PTE_IS_SWAPPED(pte) (((pte) & PTE_V) == 0 && ((pte) & PTE_SWAP))
```

### Why store original flags?

On swap-in, we need to restore the page with correct R/W/X/U permissions.
Storing them in the PTE itself avoids needing a separate per-page metadata
table.

### COW bit preservation

If a page was COW-shared before being swapped out (which should not happen per
the eviction rules -- see Section 4 -- but as defensive coding), bit 9 can
store the COW flag. However, the eviction policy explicitly avoids swapping
COW-shared pages (refcount > 1), so in practice bit 9 will be 0 for all
swapped-out PTEs.

---

## 4. Eviction Algorithm

### When to trigger eviction

Eviction is triggered from `kalloc()` when the free page count is low. Add a
free page counter to `kalloc.c`:

```c
static int free_page_count;   /* incremented by kfree, decremented by kalloc */
#define SWAP_LOW_WATERMARK  32  /* trigger eviction when below this */
#define SWAP_EVICT_BATCH     8  /* evict this many pages per trigger */
```

Modified `kalloc()`:

```c
void *kalloc(void) {
    if (free_page_count < SWAP_LOW_WATERMARK) {
        swap_evict_pages(SWAP_EVICT_BATCH);
    }
    /* ... existing allocation code ... */
}
```

### Victim selection: clock algorithm

Use a simple **clock (second-chance)** algorithm that scans across all process
page tables.

Global scan state (persistent across calls):

```c
static int clock_proc_idx = 0;     /* current process table index  */
static uint64_t clock_va = 0;      /* current virtual address       */
```

Algorithm (`swap_evict_pages(int n)`):

```
evicted = 0
while evicted < n:
    p = proc[clock_proc_idx]

    if p is UNUSED or p has no user pagetable:
        advance clock_proc_idx (wrap to 0 at 64)
        clock_va = 0
        continue

    pte = walk(p->pagetable, clock_va, 0)

    if pte is NULL or not valid or not PTE_U:
        advance clock_va by PAGE_SIZE
        if clock_va >= MAXVA:
            advance clock_proc_idx, reset clock_va = 0
        continue

    pa = PTE2PA(*pte)

    // Skip criteria:
    if page_ref_get(pa) > 1:       skip (COW shared)
    if pa is kernel page (< _kernel_end): skip
    if va is SIG_TRAMPOLINE_VA:    skip (kernel-managed trampoline)

    // Check accessed bit (second chance):
    if *pte & PTE_A:
        clear PTE_A (give second chance)
        advance clock_va
        continue

    // This page is the victim -- evict it:
    slot = swap_alloc()
    if slot < 0:
        break   // swap full, stop evicting

    swap_write(slot, (void *)pa)     // write page to disk
    orig_flags = PTE_FLAGS(*pte)
    *pte = PTE_SWAP_ENCODE(slot, orig_flags)
    sfence_vma()                     // flush TLB
    kfree((void *)pa)                // return page to free list
    evicted++
    advance clock_va
```

### What makes a page ineligible for eviction

1. **refcount > 1** -- the page is COW-shared between multiple processes.
   Evicting it would require updating PTEs in all sharing processes, which is
   complex and error-prone. Only evict pages with refcount == 1.
2. **Kernel pages** -- physical addresses below `_kernel_end` or page table
   pages (which lack PTE_U).
3. **Signal trampoline page** -- mapped at `SIG_TRAMPOLINE_VA` in every
   process; small, kernel-managed, skip it.
4. **Pages belonging to the currently running process** -- optional
   optimization to avoid swapping out pages likely to be accessed immediately
   after the fault handler returns. In a single-CPU system this is the process
   that called `kalloc()`.

---

## 5. Swap-In Path (Page Fault Handler)

The swap-in path is integrated into the existing page fault handler in
`trap.c`. The check is inserted **after** the COW fault check and **before**
the stack auto-grow and VMA demand-paging checks.

### Modified page fault handling order

```
1. COW store fault      (existing -- PTE_V=1 && PTE_COW)
2. SWAP-IN              (new -- PTE_V=0 && PTE_SWAP)
3. Stack auto-grow      (existing)
4. VMA demand paging    (existing)
5. SIGSEGV / kill       (existing fallthrough)
```

### Swap-in logic (step 2)

```c
/* In trap_handler, after COW check, for any page fault (load/store/inst): */
if (current_proc) {
    uint64_t va = PGROUNDDOWN(stval);
    pte_t *pte = walk(current_proc->pagetable, va, 0);

    if (pte && PTE_IS_SWAPPED(*pte)) {
        int slot = PTE_SWAP_SLOT(*pte);
        int flags = PTE_SWAP_FLAGS(*pte);

        /* Allocate a fresh physical page */
        char *newpage = kalloc();
        if (!newpage)
            goto pagefault_kill;   /* OOM -- cannot swap in */

        /* Read page data from swap disk */
        swap_read(slot, newpage);

        /* Free the swap slot */
        swap_free(slot);

        /* Restore the PTE with original permissions + PTE_V */
        *pte = PA2PTE((uint64_t)newpage) | flags | PTE_V;

        /* Flush TLB for this VA */
        sfence_vma_va(va);
        return;   /* resume faulting instruction */
    }
}
```

### Note on kalloc during swap-in

`kalloc()` may itself trigger eviction (Section 4). This is safe because:
- Eviction writes pages to disk using `virtio_blk_rw()`.
- Swap-in reads a page from disk using `virtio_blk_rw()`.
- These cannot overlap because `kalloc()` returns before we call `swap_read()`.
- The eviction path and the swap-in path are in the same (non-preemptible)
  kernel execution context.

However, care must be taken that the eviction triggered by `kalloc()` does
**not** try to evict the very page we are swapping in. Since the page we are
swapping in has `PTE_V=0` and `PTE_SWAP` set, the eviction scanner will skip
it (it only evicts pages with `PTE_V=1`), so this is safe.

---

## 6. Swap-Out Path

### Core I/O functions

```c
/* Write a 4 KB page to swap slot. */
void swap_write(int slot, void *page) {
    uint64_t sector = SWAP_START_SECTOR + (uint64_t)slot * SECTORS_PER_PAGE;
    /* virtio_blk_rw transfers 4096 bytes per call (one page) */
    virtio_blk_rw(sector, page, 1 /* write */);
}

/* Read a 4 KB page from swap slot. */
void swap_read(int slot, void *page) {
    uint64_t sector = SWAP_START_SECTOR + (uint64_t)slot * SECTORS_PER_PAGE;
    virtio_blk_rw(sector, page, 0 /* read */);
}
```

### Where swap-out is called

Swap-out is not called directly from user code. It is triggered in two places:

1. **From `kalloc()`** -- when `free_page_count < SWAP_LOW_WATERMARK`,
   `kalloc()` calls `swap_evict_pages()` before attempting allocation. This is
   the primary trigger.

2. **Explicit `swap_evict_pages(n)` call** -- could be called from a debugging
   syscall or a periodic timer check, but the primary mechanism is (1).

### File organization

New files:
- `kernel/swap.c` -- swap bitmap, eviction logic, swap_read/write, swap_init
- `kernel/include/swap.h` -- constants, function declarations, PTE macros

---

## 7. Integration with COW Fork and Page Fault Handler

### COW fork (`copy_pagetable_cow`)

No changes are needed to `copy_pagetable_cow()` itself, but it must be aware
of swapped-out PTEs. When forking, the function walks the parent's page table
and COW-shares pages. A swapped-out PTE (`PTE_V=0`, `PTE_SWAP=1`) should be
**copied as-is** to the child:

```c
/* In copy_pagetable_cow, after checking PTE_V: */
if (!(*pte & PTE_V)) {
    if (*pte & PTE_SWAP) {
        /* Page is on disk. Both parent and child get the same
         * swap PTE. On first access by either, the page is read
         * from disk. We must duplicate the swap slot or use a
         * swap-level refcount. See below. */
    }
    continue;  /* skip non-present, non-swap pages */
}
```

### The swap-slot sharing problem

If parent and child both have a PTE pointing to the same swap slot, and one
process faults and swaps the page in, the other process still has a PTE
pointing to that slot. Two approaches:

**Approach A (recommended): Copy-on-fork for swapped pages.** When forking, if
a PTE is swapped out, allocate a new swap slot, read the page from the old
slot into a temporary buffer, write it to the new slot, and give the child the
new slot. This is simple and avoids swap-level reference counting.

```c
if (*pte & PTE_SWAP) {
    int old_slot = PTE_SWAP_SLOT(*pte);
    int new_slot = swap_alloc();
    if (new_slot < 0) return -1;  /* swap full */

    char *tmp = kalloc();
    swap_read(old_slot, tmp);
    swap_write(new_slot, tmp);
    kfree(tmp);

    /* Child gets new slot with same flags */
    pte_t child_pte = PTE_SWAP_ENCODE(new_slot, PTE_SWAP_FLAGS(*pte));
    *child_entry = child_pte;
}
```

**Approach B: Swap-level reference counting.** Add a `swap_refcount[]` array.
On fork, increment the refcount. On swap-in, if refcount > 1, COW-share the
physical page. On process exit, decrement refcount and free slot if zero. This
is more space-efficient but more complex.

Recommendation: **Use Approach A** for simplicity. The extra disk I/O during
fork is acceptable for a teaching OS.

### COW page fault path

The existing COW fault handler checks `PTE_V && PTE_COW`. Since swapped-out
pages have `PTE_V=0`, the COW handler naturally skips them. The swap-in handler
(Section 5) restores the page with its original flags. If the original page
was marked COW before eviction (which should not happen per eviction rules),
the restored PTE would have `PTE_COW` set and `PTE_W` cleared, and a
subsequent write would trigger a normal COW fault. This is correct behavior.

### Process exit (`uvmunmap`)

The existing `uvmunmap()` skips PTEs with `PTE_V=0`. This means swapped-out
pages would leak swap slots. Modify `uvmunmap()` to also free swap slots:

```c
void uvmunmap(pagetable_t pagetable, uint64_t va, uint64_t len) {
    for (a = va; a < va + len; a += PAGE_SIZE) {
        pte = walk(pagetable, a, 0);
        if (pte == 0) continue;

        if (*pte & PTE_V) {
            kfree((void *)PTE2PA(*pte));
        } else if (*pte & PTE_SWAP) {
            swap_free(PTE_SWAP_SLOT(*pte));
        }
        *pte = 0;
    }
}
```

### Page table teardown on exec/exit

The `freewalk()` / page-table freeing code (called during `exec()` and
`exit()`) must also handle swapped-out PTEs. Any leaf PTE with `PTE_SWAP` set
must have its swap slot freed before the PTE is zeroed and the page table page
is reclaimed.

---

## 8. Avoiding Conflicts with Filesystem I/O

### The reentrancy problem

`virtio_blk_rw()` uses **static** local variables (`static struct
virtio_blk_req req` and `static uint8_t status_byte`). If two calls to
`virtio_blk_rw()` overlap, the static request struct is corrupted.

### Why this is safe in the current design

SBUnix is a **single-hart** (single CPU) system. The kernel is non-preemptive:
once a trap handler begins executing, it runs to completion before another
process can be scheduled. Specifically:

1. **Timer preemption** calls `swtch()` back to the scheduler, which only
   resumes a process via `swtch()` -- it never re-enters the same kernel
   execution path.

2. **Swap I/O** happens in two contexts:
   - **Eviction** (from `kalloc()`, called during kernel-mode execution)
   - **Swap-in** (from the page fault handler, also kernel-mode execution)

   Both run with interrupts managed by the trap infrastructure. No other code
   path calls `virtio_blk_rw()` concurrently because there is only one hart
   and kernel execution is non-preemptive.

3. **The tarfs filesystem** does not use `virtio_blk_rw()` at all -- it reads
   from the in-memory `.tarfs` section. There is no filesystem I/O to conflict
   with.

### Guardrails to add anyway

Even though reentrancy cannot happen today, add a defensive assertion:

```c
static int blk_io_in_progress = 0;

int virtio_blk_rw(uint64_t sector, void *buf, int write) {
    if (blk_io_in_progress)
        panic("virtio_blk_rw: reentrant call detected");
    blk_io_in_progress = 1;
    /* ... existing code ... */
    blk_io_in_progress = 0;
    return 0;
}
```

### Future considerations

If SBUnix adds a disk-based filesystem (ext2, FAT, etc.) that shares the same
virtio-blk device, a proper **block I/O lock** or request queue will be
needed. For now, the static request struct and single-hart model make this
unnecessary.

---

## 9. Edge Cases and Pitfalls

### 9.1 Swap space exhaustion

If `swap_alloc()` returns -1 during eviction, `swap_evict_pages()` stops
evicting. If `kalloc()` still cannot allocate a page after eviction, it returns
NULL. The caller (page fault handler, `uvmalloc()`, etc.) must handle this
gracefully -- typically by killing the process with SIGSEGV or returning an
error to the syscall.

### 9.2 Eviction during swap-in

When the page fault handler calls `kalloc()` to allocate a page for swap-in,
`kalloc()` may trigger eviction. The eviction scanner must not evict pages from
the **currently faulting process's page table** in a way that creates a
circular dependency. This is safe because:
- The faulting page has `PTE_V=0` (it is being swapped in), so the scanner
  skips it.
- Other pages in the same process may be evicted, which is fine.

### 9.3 Evicting a page that is part of a multi-page I/O buffer

`virtio_blk_rw()` takes a `void *buf` pointer that must remain valid for the
duration of I/O. Since swap I/O is synchronous (polled), the buffer page
cannot be evicted during I/O. The eviction scanner runs from `kalloc()`,
which is called before the I/O begins, so no conflict exists.

### 9.4 Swapping out the trapframe or kernel stack

The trapframe and kernel stack are allocated with `kalloc()` but are not mapped
with `PTE_U`. The eviction scanner only targets pages with `PTE_U` set, so
these are safe. Additionally, `page_ref_get()` would return 1 for these pages,
but since they have no user PTE pointing to them, the scanner never encounters
them.

### 9.5 Page table pages

Intermediate page table pages (level 1 and level 2) have `PTE_V` set but no
R/W/X bits -- they are "pointer" PTEs. The scanner should only evict **leaf**
PTEs (those with at least one of R, W, X set). Add an explicit check:

```c
if ((*pte & (PTE_R | PTE_W | PTE_X)) == 0)
    continue;  /* not a leaf PTE, skip */
```

### 9.6 TLB consistency

After modifying a PTE (either clearing it during eviction or restoring it
during swap-in), a TLB flush is mandatory. Use `sfence.vma` with the specific
VA when possible:

```c
asm volatile("sfence.vma %0, zero" : : "r"(va));
```

On a single-hart system, a per-VA flush is sufficient. A full TLB flush
(`sfence.vma zero, zero`) is acceptable but less efficient.

### 9.7 Dirty pages

The eviction scanner does not distinguish between dirty and clean pages. All
evicted pages are written to disk unconditionally. An optimization would be to
check the PTE_D (dirty) bit and skip the disk write for clean pages, but this
adds complexity (tracking which pages have swap backing) and is unnecessary
for a teaching OS.

### 9.8 Process exit with swapped-out pages

When a process exits, `uvmunmap()` (and the page table teardown code) must
free swap slots for any swapped-out PTEs. Failing to do so leaks swap slots.
See Section 7 for the required modification to `uvmunmap()`.

### 9.9 exec() replacing the address space

When `exec()` tears down the old page table and builds a new one, swapped-out
PTEs in the old page table must have their swap slots freed. The same
modification to `uvmunmap()` / page table teardown handles this.

### 9.10 Double-free of swap slots

Ensure that `swap_free()` asserts the slot was actually in use:

```c
void swap_free(int slot) {
    if (!swap_is_used(slot))
        panic("swap_free: double free of slot");
    /* clear bit in bitmap */
}
```

### 9.11 kalloc returning NULL even after eviction

If eviction fails (swap full) and `kalloc()` returns NULL, the system is
truly out of memory. The page fault handler should deliver SIGSEGV. Syscalls
like `mmap()` or `sbrk()` should return -ENOMEM. This matches the existing
behavior where `kalloc()` returning NULL leads to process termination.

---

## 10. Testing Approach

### 10.1 Unit test: swap bitmap

Write a test that allocates and frees swap slots, verifying:
- `swap_alloc()` returns unique slot indices 0, 1, 2, ...
- `swap_free(slot)` makes the slot available for reallocation
- Allocating `SWAP_NSLOTS` slots exhausts swap; next `swap_alloc()` returns -1
- After freeing one slot, `swap_alloc()` returns that slot

### 10.2 Unit test: PTE encoding round-trip

Verify that `PTE_SWAP_ENCODE` / `PTE_SWAP_SLOT` / `PTE_SWAP_FLAGS` are
consistent:

```c
for (int slot = 0; slot < SWAP_NSLOTS; slot++) {
    int flags = PTE_R | PTE_W | PTE_U;
    pte_t pte = PTE_SWAP_ENCODE(slot, flags);
    assert(PTE_IS_SWAPPED(pte));
    assert(PTE_SWAP_SLOT(pte) == slot);
    assert(PTE_SWAP_FLAGS(pte) == flags);
}
```

### 10.3 Integration test: memory pressure with single process

Write a user program that allocates more memory than physically available
(e.g., `mmap` 64 MB in a 128 MB system where the kernel uses ~30 MB). Touch
each page. If swap works, the program completes without being killed. If swap
is broken, the process receives SIGSEGV.

```c
/* bin/swaptest/swaptest.c */
#define NPAGES 8192   /* 32 MB */
int main() {
    char *mem = mmap(NULL, NPAGES * 4096, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANON, -1, 0);
    /* Touch every page */
    for (int i = 0; i < NPAGES; i++)
        mem[i * 4096] = (char)(i & 0xff);

    /* Verify every page */
    for (int i = 0; i < NPAGES; i++) {
        if (mem[i * 4096] != (char)(i & 0xff)) {
            write(2, "FAIL\n", 5);
            exit(1);
        }
    }
    write(1, "PASS\n", 5);
    exit(0);
}
```

### 10.4 Integration test: fork with swap pressure

Fork a process that has already touched many pages, then have both parent and
child continue accessing their pages. Verifies that COW + swap interaction
works correctly.

### 10.5 Integration test: multiple processes under pressure

Launch several processes that each allocate and touch a moderate amount of
memory, forcing the system to swap pages across multiple address spaces.
Verify all processes complete correctly.

### 10.6 Stress test: swap thrashing

Allocate memory far exceeding physical RAM + swap. Access pages in a pattern
that forces continuous swap-in/swap-out. Verify the system remains stable
(does not panic) and that processes that exceed total available memory
(physical + swap) are killed cleanly with SIGSEGV.

### 10.7 Regression test: existing functionality

After implementing swap, re-run all existing test programs (spin, alarmtest,
segvtest, shell commands) to verify nothing is broken. The swap mechanism
should be transparent to programs that do not exceed physical memory.

### 10.8 Debugging aids

Add a `swap_stats()` function callable from a debug syscall or printk:

```c
void swap_stats(void) {
    printk("swap: %d/%d slots used, free_pages=%d\n",
           swap_used_count, SWAP_NSLOTS, free_page_count);
}
```

Call this periodically from the timer handler (guarded by a debug flag) to
monitor swap activity during development.

---

## Appendix: Summary of Files to Create or Modify

### New files
| File | Purpose |
|------|---------|
| `kernel/swap.c` | Swap bitmap, eviction clock, swap_read/write, swap_init |
| `kernel/include/swap.h` | SWAP_NSLOTS, PTE_SWAP macros, function declarations |

### Modified files
| File | Change |
|------|--------|
| `kernel/include/riscv.h` | Add `PTE_SWAP` define (same bit as PTE_COW, distinct by PTE_V state) |
| `kernel/kalloc.c` | Add `free_page_count` tracking; call `swap_evict_pages()` from `kalloc()` when low |
| `kernel/vm.c` | Modify `uvmunmap()` to free swap slots for swapped-out PTEs |
| `kernel/trap.c` | Add swap-in check in page fault handler (after COW, before stack grow) |
| `kernel/trap.c` | Modify `copy_pagetable_cow()` to handle swapped-out PTEs during fork |
| `kernel/virtio_blk.c` | Add reentrancy assertion to `virtio_blk_rw()` |
| `kernel/kernel.c` | Call `swap_init()` during boot (after `virtio_blk_init()`) |
