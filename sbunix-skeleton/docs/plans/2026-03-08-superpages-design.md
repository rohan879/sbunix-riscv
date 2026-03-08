# Sv39 Super Page (2MB Megapage) Support -- Design Document

Date: 2026-03-08
Branch: `scheduler` (feature branch)
Status: Design phase -- no implementation code

---

## 1. Background

RISC-V Sv39 supports three page sizes via leaf PTEs at different levels of the
three-level radix tree:

| Level | Page size | PPN alignment requirement |
|-------|-----------|---------------------------|
| 0     | 4 KB      | 4 KB (always satisfied)   |
| 1     | 2 MB      | PPN\[0\] (bits 18:10) must be zero, i.e. PA is 2 MB-aligned |
| 2     | 1 GB      | PPN\[1:0\] must be zero, i.e. PA is 1 GB-aligned |

A **leaf PTE** is one where at least one of R, W, X is set. The current kernel
only creates leaf PTEs at level 0. A leaf PTE at level 1 maps a contiguous
2 MB (512 x 4 KB) region -- a "megapage" or "super page."

The RISC-V privileged spec (Section 4.4.1) states: if a leaf PTE is found at
level `i > 0` and `pa.ppn[i-1:0] != 0`, then a page-fault exception is raised.
This means the physical address backing a level-1 leaf **must** be 2 MB-aligned
(`pa & 0x1FFFFF == 0`).

---

## 2. Current Codebase Summary

### 2.1 `kernel/kalloc.c` (lines 1-93)

- Free list allocator for 4 KB pages. `kalloc()` pops one `struct run` from
  `kmem.freelist`; `kfree()` pushes one back (respecting refcounts).
- `page_refcount[MAX_PAGES]` (32768 entries for 128 MB) tracks per-4KB-page
  reference counts. Indexed by `pa_to_index(pa) = (pa - 0x80000000) / 4096`.
- `kinit()` (lines 64-78) walks from `_kernel_end` (page-aligned) to `PHYSTOP`
  adding every 4 KB page to the free list.
- **No support for multi-page contiguous allocation.**

### 2.2 `kernel/vm.c` (lines 1-334)

- **`walk()` (lines 14-27):** Walks levels 2 -> 1 -> 0, always descending to
  level 0. If a PTE is valid at level 2 or 1, it unconditionally treats it as a
  page-table pointer (`PTE2PA`) and descends. It never checks whether the PTE
  is a leaf (has R/W/X bits set). Returns `&pagetable[PX(0, va)]`.
- **`mappages()` (lines 32-51):** Iterates in 4 KB (`PAGE_SIZE`) steps, calling
  `walk(..., 1)` for each page. Always creates level-0 leaf PTEs.
- **`kvmmap()` / `kvminit()` (lines 56-90):** Identity-maps kernel RAM
  (0x80000000 to PHYSTOP), UART, PCIe, and PLIC regions using `mappages()`.
  All mappings are 4 KB granularity even for large regions.
- **`uvmunmap()` (lines 133-145):** Iterates in 4 KB steps, calls
  `walk(..., 0)` to find level-0 PTEs, frees the backing page.
- **`uvmalloc()` (lines 104-124):** Allocates page-at-a-time.
- **`uvmcopyin()` (lines 312-334):** Copies data into user pages, 4 KB at a
  time using `walk()`.

### 2.3 `kernel/include/vm.h` (lines 1-45)

- Declares `walk`, `mappages`, `kvmmap`, `uvmunmap`, etc.
- Defines VMA structures and `PROT_*` / `MAP_*` constants.
- No super-page-related definitions.

### 2.4 `kernel/include/riscv.h` (lines 1-122)

- PTE flag definitions: `PTE_V`, `PTE_R`, `PTE_W`, `PTE_X`, `PTE_U`,
  `PTE_COW` (RSW bit 8).
- Macros: `PA2PTE`, `PTE2PA`, `PTE_FLAGS`, `PX(level, va)`, `PXSHIFT`.
- `PGROUNDDOWN`, `PGROUNDUP` -- 4 KB aligned only.
- No `SUPERPAGE_SIZE`, no 2 MB alignment macros.

### 2.5 `kernel/include/kalloc.h` (lines 1-17)

- `PAGE_SIZE 4096`, `kalloc()`, `kfree()`, refcount functions.

### 2.6 `kernel/trap.c` -- COW fault handler (lines 630-658)

- On `SCAUSE_STORE_PAGE_FAULT`, checks for `PTE_COW` on the faulting PTE.
- If `page_ref_get(pa) == 1`: removes COW, adds PTE_W (in-place).
- Otherwise: `kalloc()` a new 4 KB page, `memcpy` 4 KB, update PTE, `kfree`
  old page (decrements refcount).
- Flushes TLB for the faulting VA only.

### 2.7 `kernel/trap.c` -- `copy_pagetable_cow()` (lines 276-303)

- Iterates over user VA space in 4 KB steps (0 to `sz`).
- For each valid `PTE_U` page: if writable, clears `PTE_W`, sets `PTE_COW` in
  both parent and child; bumps `page_ref_inc(pa)`.
- Always calls `mappages(dst, va, PAGE_SIZE, pa, flags)`.

### 2.8 `kernel/kernel.c` -- `boot()` (lines 19-47)

- Calls `kinit()` then `kvminit()`. Kernel page table is set up before any
  processes run.

---

## 3. Design Overview

### 3.1 Constants and Macros

**File: `kernel/include/riscv.h`**

Add after the existing `PGROUNDDOWN` macro (after line 25):

```
#define SUPERPAGE_SIZE   (512 * PAGE_SIZE)           /* 2 MB = 0x200000 */
#define SUPERPAGE_SHIFT  21
#define SUPERPAGE_MASK   (SUPERPAGE_SIZE - 1)        /* 0x1FFFFF */
#define SPGROUNDDOWN(a)  ((a) & ~(uint64_t)SUPERPAGE_MASK)
#define SPGROUNDUP(a)    (((a) + SUPERPAGE_MASK) & ~(uint64_t)SUPERPAGE_MASK)
#define IS_SUPERPAGE_ALIGNED(a)  (((uint64_t)(a) & SUPERPAGE_MASK) == 0)
```

Also add a helper to detect leaf PTEs:

```
/* A PTE is a leaf if any of R, W, X is set (Sv39 spec) */
#define PTE_IS_LEAF(pte)  ((pte) & (PTE_R | PTE_W | PTE_X))
```

**File: `kernel/include/kalloc.h`**

Add after line 10:

```
#define SUPERPAGE_NPAGES 512   /* number of 4KB pages in one 2MB super page */
void *kalloc_super(void);      /* allocate 2MB-aligned contiguous block */
void  kfree_super(void *pa);   /* free a 2MB contiguous block */
```

**File: `kernel/include/vm.h`**

Add after the `mappages` declaration (after line 36):

```
int  mappages_super(pagetable_t pagetable, uint64_t va, uint64_t size,
                    uint64_t pa, int perm);
pte_t *walk_super(pagetable_t pagetable, uint64_t va, int *level_out);
void  superpage_split(pagetable_t pagetable, uint64_t va);
```

---

## 4. Detailed Changes by File

### 4.1 `kernel/kalloc.c` -- Super Page Allocator

**New function: `kalloc_super()`** (add after `kalloc()`, around line 92)

Strategy: scan the free list for 512 contiguous 4 KB pages starting at a
2 MB-aligned physical address.

Algorithm:
1. Walk the free list and build a bitmap or sorted array of free page indices.
2. For each 2 MB-aligned base index `b` (i.e., `b % 512 == 0`), check whether
   all 512 pages `[b, b+512)` are on the free list.
3. If found, remove all 512 pages from the free list and set
   `page_refcount[b]` to 1 (the base page tracks the super-page refcount).
   Set `page_refcount[b+1..b+511]` to a sentinel value (e.g., -1 meaning
   "part of super page, see base") -- or simply leave them at 0 and only use
   the base for reference counting.
4. Return `(void *)(0x80000000 + b * PAGE_SIZE)`.
5. If no contiguous 2 MB block is found, return `NULL`.

Performance note: this scan is O(N) where N = number of free pages (up to
32768). At boot time with 128 MB RAM this is cheap. At runtime super-page
allocation is expected to be rare.

Alternative (buddy allocator): A full buddy allocator would make this O(1) but
is a much larger change. For a teaching OS, the scan approach is acceptable.
If fragmentation becomes a concern, `kinit()` could be modified to pre-identify
2 MB-aligned free blocks and maintain a separate `super_freelist`.

**New function: `kfree_super(void *pa)`** (add after `kalloc_super()`)

1. Assert `IS_SUPERPAGE_ALIGNED(pa)`.
2. Return all 512 constituent 4 KB pages to `kmem.freelist` via `kfree()` on
   each one (after setting their refcount to 1 so `kfree` drops them to 0 and
   enqueues them).

**Refcount strategy for super pages:**

- The base page (index `b`) holds the refcount for the entire 2 MB block.
- Pages `b+1` through `b+511` have their `page_refcount` set to 0 (unused) or
  a sentinel like `SUPER_PAGE_MEMBER = -1` to catch erroneous per-page kfree.
- New helpers:
  - `page_ref_inc_super(pa)`: increments `page_refcount[pa_to_index(pa)]`
    where `pa` must be the super-page base.
  - `page_ref_get_super(pa)`: returns `page_refcount[pa_to_index(pa)]`.
  - `page_ref_dec_super(pa)`: decrements; if it reaches 0, the super page is
    freed.

**Modification to `kfree()` (line 44):**

Add a guard: if `page_refcount[idx]` is the sentinel value (`-1`), print a
warning and return without freeing. This prevents accidental per-page free of
memory that is part of a live super page.

**Modification to `kinit()` (lines 64-78):**

Optionally, after the per-page free loop, scan for contiguous 2 MB-aligned
blocks and move them to a `super_freelist`. This is an optimization and can be
deferred.

---

### 4.2 `kernel/vm.c` -- Page Table Walk and Mapping

#### 4.2.1 `walk()` changes (lines 14-27)

The current `walk()` always descends through levels 2 and 1, assuming every
valid PTE at those levels is a page-table pointer. With super pages, a valid
PTE at level 1 might be a **leaf** (it has R, W, or X set). Walking into it as
a page table would interpret data memory as PTEs -- a critical bug.

**Modified `walk()` -- option A (backward-compatible):**

Keep the existing `walk()` signature, but add a leaf-PTE check in the loop:

```
pte_t *walk(pagetable_t pagetable, uint64_t va, int alloc) {
    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pagetable[PX(level, va)];
        if (*pte & PTE_V) {
            if (PTE_IS_LEAF(*pte)) {
                // This is a super page (level 1) or giga page (level 2).
                // Cannot descend further. Return this PTE.
                return pte;
            }
            pagetable = (pagetable_t)PTE2PA(*pte);
        } else {
            if (!alloc || (pagetable = (pagetable_t)kalloc()) == 0)
                return 0;
            memset(pagetable, 0, PAGE_SIZE);
            *pte = PA2PTE(pagetable) | PTE_V;
        }
    }
    return &pagetable[PX(0, va)];
}
```

This is **almost** backward compatible. The concern: callers that get back a
level-1 PTE may misinterpret the returned pointer (e.g., `uvmcopyin` assumes
the returned PTE maps 4 KB). Therefore we also need:

**New function: `walk_super()` -- returns level information:**

```
pte_t *walk_super(pagetable_t pagetable, uint64_t va, int *level_out) {
    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pagetable[PX(level, va)];
        if (*pte & PTE_V) {
            if (PTE_IS_LEAF(*pte)) {
                if (level_out) *level_out = level;
                return pte;
            }
            pagetable = (pagetable_t)PTE2PA(*pte);
        } else {
            if (level_out) *level_out = -1;
            return 0;
        }
    }
    if (level_out) *level_out = 0;
    return &pagetable[PX(0, va)];
}
```

This variant does NOT allocate intermediate page tables (it is a lookup-only
function). Callers that need to distinguish super pages from regular pages
should use `walk_super()`.

#### 4.2.2 `mappages()` changes (lines 32-51)

The existing `mappages()` is unchanged -- it continues to create 4 KB mappings
at level 0. All existing callers (user page allocation, signal trampoline,
etc.) keep working.

**New function: `mappages_super()`:**

```
int mappages_super(pagetable_t pagetable, uint64_t va, uint64_t size,
                   uint64_t pa, int perm) {
    // va, pa must be 2MB-aligned; size must be a multiple of 2MB
    assert(IS_SUPERPAGE_ALIGNED(va));
    assert(IS_SUPERPAGE_ALIGNED(pa));
    assert(IS_SUPERPAGE_ALIGNED(size));

    uint64_t end = va + size;
    for (; va < end; va += SUPERPAGE_SIZE, pa += SUPERPAGE_SIZE) {
        // Walk to level 2, install level-1 PTE ourselves
        pte_t *l2pte = &pagetable[PX(2, va)];
        pagetable_t l1pt;
        if (*l2pte & PTE_V) {
            l1pt = (pagetable_t)PTE2PA(*l2pte);
        } else {
            l1pt = (pagetable_t)kalloc();
            if (!l1pt) return -1;
            memset(l1pt, 0, PAGE_SIZE);
            *l2pte = PA2PTE(l1pt) | PTE_V;
        }

        pte_t *l1pte = &l1pt[PX(1, va)];
        if (*l1pte & PTE_V) {
            // already mapped -- skip or error depending on policy
        }
        // Install leaf PTE at level 1
        *l1pte = PA2PTE(pa) | perm | PTE_V;
    }
    return 0;
}
```

Key difference from `mappages()`: we stop at level 1 and install the leaf PTE
there. We do not create a level-0 page table.

#### 4.2.3 `uvmunmap()` changes (lines 133-145)

Current code iterates in 4 KB steps. Must detect super-page PTEs.

Modified approach:

```
void uvmunmap(pagetable_t pagetable, uint64_t va, uint64_t len) {
    uint64_t a = va;
    while (a < va + len) {
        int level;
        pte_t *pte = walk_super(pagetable, a, &level);
        if (!pte || !(*pte & PTE_V)) {
            a += PAGE_SIZE;    // skip unmapped 4KB region
            continue;
        }
        uint64_t pa = PTE2PA(*pte);
        if (level == 1) {
            // Super page: free the 2MB block
            kfree_super((void *)pa);
            *pte = 0;
            a += SUPERPAGE_SIZE;
        } else {
            // Regular 4KB page
            kfree((void *)pa);
            *pte = 0;
            a += PAGE_SIZE;
        }
    }
}
```

#### 4.2.4 `uvmcopyin()` changes (lines 312-334)

Must handle the case where `walk()` returns a super-page PTE. The physical
address for a VA within a super page is:

```
pa_base = PTE2PA(*pte);
offset  = va & SUPERPAGE_MASK;
pa      = pa_base + offset;
```

Modified to use `walk_super()` and compute offset correctly:

```
int uvmcopyin(pagetable_t pagetable, uint64_t dstva, char *src, uint64_t len) {
    while (len > 0) {
        int level;
        uint64_t va0, pa0, n;
        uint64_t page_size;

        if we use walk_super:
            pte_t *pte = walk_super(pagetable, dstva, &level);
            ...
            page_size = (level == 1) ? SUPERPAGE_SIZE : PAGE_SIZE;
            va0 = (level == 1) ? SPGROUNDDOWN(dstva) : PGROUNDDOWN(dstva);
            pa0 = PTE2PA(*pte) + (dstva - va0);

        n = page_size - (dstva - va0);
        if (n > len) n = len;
        memcpy((void *)pa0, src, n);
        len -= n;
        src += n;
        dstva = va0 + page_size;
    }
    return 0;
}
```

---

### 4.3 `kernel/trap.c` -- COW Fork

#### 4.3.1 COW Strategy Decision: Split on Fork

Two strategies are possible:

**Strategy A -- Split on Fork (chosen):**
When `copy_pagetable_cow()` encounters a super page in the source, it breaks
the super page into 512 individual 4 KB COW mappings in both parent and child.

Pros:
- The COW fault handler remains simple (4 KB granularity only).
- No risk of wasting 2 MB when only one byte is written.
- Matches what Linux does for transparent huge pages on fork.

Cons:
- Fork becomes slower for processes with many super pages (but user processes
  are unlikely to have super pages in this teaching OS -- super pages are
  primarily used for kernel identity mappings).
- Splitting requires allocating page tables (up to 1 level-0 page table per
  super page = 4 KB per 2 MB, i.e., 0.2% overhead).

**Strategy B -- COW Entire 2 MB Block (rejected for initial implementation):**
Keep the super-page mapping in both parent and child, mark it COW. On fault,
copy the entire 2 MB block.

Pros:
- Fork stays fast for large super-page regions.
- Maintains TLB efficiency until a write occurs.

Cons:
- A single-byte write triggers a 2 MB copy.
- Requires `kalloc_super()` in the page fault handler, which may fail due to
  fragmentation.
- Adds significant complexity to the fault handler.
- The 2 MB refcount model is harder to get right.

**Verdict:** Use Strategy A (split on fork). It is simpler, and super pages in
this OS will primarily be kernel identity mappings, which are not COW-forked
(they are re-created by `sys_clone` via direct `mappages` calls -- see
trap.c lines 332-336).

#### 4.3.2 `copy_pagetable_cow()` changes (trap.c lines 276-303)

Currently iterates in `PAGE_SIZE` steps. Must detect super-page PTEs.

Modified approach:

```
static int copy_pagetable_cow(pagetable_t dst, pagetable_t src, uint64_t sz) {
    uint64_t va = 0;
    while (va < sz) {
        int level;
        pte_t *pte = walk_super(src, va, &level);
        if (!pte || !(*pte & PTE_V)) {
            va += PAGE_SIZE;
            continue;
        }
        if (!(*pte & PTE_U)) {
            va += (level == 1) ? SUPERPAGE_SIZE : PAGE_SIZE;
            continue;
        }

        if (level == 1) {
            // Super page: split into 512 individual 4KB COW pages
            uint64_t base_pa = PTE2PA(*pte);
            int base_flags = PTE_FLAGS(*pte);
            if (base_flags & PTE_W) {
                base_flags = (base_flags & ~PTE_W) | PTE_COW;
            }

            // Remove the super-page PTE from parent
            *pte = 0;

            // Create 512 individual level-0 PTEs in parent + child
            for (int i = 0; i < 512; i++) {
                uint64_t sub_va = va + i * PAGE_SIZE;
                uint64_t sub_pa = base_pa + i * PAGE_SIZE;

                // Set per-page refcount to 2 (parent + child)
                page_refcount[pa_to_index(sub_pa)] = 2;

                // Map in parent (level-0 PTE)
                mappages(src, sub_va, PAGE_SIZE, sub_pa, base_flags);
                // Map in child (level-0 PTE)
                mappages(dst, sub_va, PAGE_SIZE, sub_pa, base_flags);
            }
            va += SUPERPAGE_SIZE;
        } else {
            // Regular 4KB page: existing COW logic
            uint64_t pa = PTE2PA(*pte);
            int flags = PTE_FLAGS(*pte);
            if (flags & PTE_W) {
                *pte = PA2PTE(pa) | ((flags & ~PTE_W) | PTE_COW | PTE_V);
                flags = (flags & ~PTE_W) | PTE_COW;
            }
            page_ref_inc(pa);
            mappages(dst, va, PAGE_SIZE, pa, flags);
            va += PAGE_SIZE;
        }
    }
    sfence_vma();
    return 0;
}
```

Note: splitting a kernel super page's refcount requires setting each
constituent 4 KB page's refcount individually. Since the 512 pages were
previously tracked as one block (only the base had refcount=1, others had 0
or -1), we must initialize all 512 refcounts to 2 (one for parent, one for
child).

#### 4.3.3 Page Fault Handler Changes (trap.c lines 635-658)

If we adopt Strategy A (split on fork), the COW fault handler remains
**unchanged**. It only ever sees level-0 PTEs with `PTE_COW` set, and the
existing 4 KB copy logic handles them.

However, we must still update `walk()` to not crash on super-page PTEs. The
fault handler calls `walk(current_proc->pagetable, va, 0)` at line 637. With
the modified `walk()` from section 4.2.1, this will correctly return the
level-1 leaf PTE if the faulting address is in a super page. But since
user-space super pages are split on fork (before any COW fault can occur), this
path is only exercised for non-COW super pages, where the handler would fall
through to the generic fault path. No additional changes needed.

**Future enhancement (Strategy B):** If we later want COW for entire super
pages, the fault handler would need:
1. Detect `PTE_COW` on a level-1 PTE.
2. Call `kalloc_super()` for a new 2 MB block.
3. `memcpy` 2 MB.
4. Update the PTE to point to the new block.
5. `kfree_super()` on the old block if refcount drops to 0.
6. If `kalloc_super()` fails, fall back to splitting the super page into 512
   4 KB pages and retrying the fault.

---

### 4.4 `kernel/kernel.c` / `kernel/vm.c` -- Kernel Mapping Setup

#### 4.4.1 `kvminit()` changes (vm.c lines 62-90)

Replace selected 4 KB mappings with 2 MB super-page mappings where alignment
permits.

Analysis of current kernel mappings:

| Region              | Base       | Size       | 2MB-aligned base? | 2MB-aligned size? | Super-page candidate? |
|---------------------|------------|------------|--------------------|--------------------|----------------------|
| UART                | 0x10000000 | 0x1000     | Yes (256 MB)       | No (4 KB)          | No (too small)       |
| PCIe ECAM           | 0x30000000 | 0x10000000 | Yes                | Yes (256 MB)       | Yes (128 super pages) |
| PCIe MMIO           | 0x40000000 | 0x40000000 | Yes                | Yes (1 GB)         | Yes (512 super pages) |
| PLIC region 1       | 0x0c000000 | 0x400000   | Yes (192 MB)       | Yes (4 MB)         | Yes (2 super pages)  |
| PLIC region 2       | 0x0c200000 | 0x400000   | Yes (2 MB-aligned) | Yes (4 MB)         | Yes (2 super pages)  |
| Kernel RAM          | 0x80000000 | 0x8000000  | Yes (2 GB)         | Yes (128 MB)       | Yes (64 super pages) |

Modified `kvminit()`:

```
void kvminit(void) {
    kernel_pagetable = (pagetable_t)kalloc();
    memset(kernel_pagetable, 0, PAGE_SIZE);

    // UART: too small for super page, use 4KB
    kvmmap(0x10000000, 0x10000000, PAGE_SIZE, PTE_R | PTE_W);

    // PCIe ECAM: 256MB, 2MB-aligned -> use super pages
    kvmmap_super(0x30000000, 0x30000000, 0x10000000, PTE_R | PTE_W);

    // PCIe MMIO: 1GB, 2MB-aligned -> use super pages
    kvmmap_super(0x40000000, 0x40000000, 0x40000000, PTE_R | PTE_W);

    // PLIC: 4MB each, 2MB-aligned -> use super pages
    kvmmap_super(0x0c000000, 0x0c000000, 0x400000, PTE_R | PTE_W);
    kvmmap_super(0x0c200000, 0x0c200000, 0x400000, PTE_R | PTE_W);

    // Kernel RAM: 128MB from 0x80000000, 2MB-aligned -> use super pages
    kvmmap_super(0x80000000, 0x80000000, PHYSTOP - 0x80000000, PTE_R | PTE_W | PTE_X);

    w_satp(MAKE_SATP(kernel_pagetable));
    sfence_vma();
    printk("kvminit: paging enabled (with 2MB super pages).\n");
}
```

**New function: `kvmmap_super()`:**

```
void kvmmap_super(uint64_t va, uint64_t pa, uint64_t sz, int perm) {
    if (mappages_super(kernel_pagetable, va, sz, pa, perm) != 0)
        printk("kvmmap_super failed\n");
}
```

**Critical note on kernel identity mappings:** These are identity maps (VA=PA)
for MMIO and RAM. We do NOT call `kalloc_super()` here because we are not
allocating new physical memory -- we are just mapping existing physical
addresses into the page table. `mappages_super()` only allocates page-table
pages (level-1 tables), not the mapped memory itself. This is the same as what
`mappages()` does today (it allocates only page-table pages via `walk()`).

#### 4.4.2 `sys_clone()` identity maps (trap.c lines 332-336)

The child process re-creates kernel identity mappings. These should also use
super pages:

```
// Replace:
mappages(new_pt, 0x80000000, PHYSTOP - 0x80000000, 0x80000000, PTE_R|PTE_W|PTE_X);
// With:
mappages_super(new_pt, 0x80000000, PHYSTOP - 0x80000000, 0x80000000, PTE_R|PTE_W|PTE_X);
```

Similarly for PCIe and PLIC mappings in `sys_clone()`. The UART mapping at
0x10000000 stays as a 4 KB mapping.

---

## 5. New Functions Summary

| Function | File | Purpose |
|----------|------|---------|
| `kalloc_super()` | `kernel/kalloc.c` | Allocate 512 contiguous 4KB pages (2MB-aligned) |
| `kfree_super(void *pa)` | `kernel/kalloc.c` | Free a 2MB block back to the free list |
| `walk_super(pt, va, &level)` | `kernel/vm.c` | Walk page table, return PTE + level (no alloc) |
| `mappages_super(pt, va, sz, pa, perm)` | `kernel/vm.c` | Create level-1 leaf PTEs (2MB mappings) |
| `kvmmap_super(va, pa, sz, perm)` | `kernel/vm.c` | Convenience wrapper for kernel super-page maps |
| `superpage_split(pt, va)` | `kernel/vm.c` | Demote one 2MB PTE into 512 4KB PTEs |

### `superpage_split()` detail

Used by `copy_pagetable_cow()` and potentially by `mprotect` or `munmap` on a
partial super page.

```
void superpage_split(pagetable_t pagetable, uint64_t va) {
    // va must be 2MB-aligned
    int level;
    pte_t *pte = walk_super(pagetable, va, &level);
    if (!pte || level != 1 || !(*pte & PTE_V))
        return; // not a super page

    uint64_t pa = PTE2PA(*pte);
    int flags = PTE_FLAGS(*pte);

    // Clear the super page PTE
    *pte = 0;

    // Create 512 level-0 PTEs
    for (int i = 0; i < 512; i++) {
        uint64_t sub_va = va + i * PAGE_SIZE;
        uint64_t sub_pa = pa + i * PAGE_SIZE;
        // Set refcount for each sub-page (was tracked as one block)
        page_refcount[pa_to_index(sub_pa)] = 1;
        mappages(pagetable, sub_va, PAGE_SIZE, sub_pa, flags);
    }
    sfence_vma();
}
```

---

## 6. Edge Cases and Pitfalls

### 6.1 Misaligned super-page PPN

If `PA2PTE(pa)` is called with a PA that is not 2 MB-aligned, the resulting
PTE will have nonzero `ppn[0]` bits. The RISC-V hardware will raise a
page fault on access. `mappages_super()` must assert alignment.

### 6.2 `walk()` returning a level-1 PTE to callers expecting level-0

After modifying `walk()` to detect leaf PTEs, every caller must be audited:

| Caller | File:Line | Current assumption | Impact |
|--------|-----------|-------------------|--------|
| `mappages()` | vm.c:39 | walk returns level-0 PTE | walk is called with `alloc=1`; if a super page already covers the VA, walk returns the level-1 leaf. `mappages()` would then overwrite the super-page PTE. Fix: `mappages()` should detect this (via `PTE_IS_LEAF` on a level > 0 PTE) and fail or split. |
| `uvmunmap()` | vm.c:138 | walk returns level-0 PTE | Must handle level-1 (addressed in section 4.2.3) |
| `uvmcopyin()` | vm.c:318 | Offset within 4KB page | Must compute offset within 2MB (addressed in section 4.2.4) |
| `uvmalloc()` | vm.c:112 | Allocates 4KB pages | No super pages here (user allocation) -- safe |
| COW handler | trap.c:637 | PTE is level-0 | Safe if we split on fork (no user super pages with COW) |
| Stack grow | trap.c:666 | 4KB allocation | Safe (stack never uses super pages) |
| Demand paging | trap.c:688 | 4KB allocation | Safe (VMAs use 4KB pages) |
| `copy_pagetable_cow()` | trap.c:279 | walk returns level-0 PTE | Must handle level-1 (addressed in section 4.3.2) |

### 6.3 TLB flushing

After splitting a super page, we must do a **full TLB flush** (`sfence.vma
zero, zero`) or flush all 512 individual VAs. A single-VA flush is not
sufficient because the TLB may have cached the 2 MB mapping. The RISC-V spec
guarantees that `sfence.vma zero, zero` flushes all entries.

After creating a super-page mapping, a `sfence.vma` is needed if any prior
4 KB mapping existed for the same VA range (unlikely in practice since we create
super pages at boot before any access).

### 6.4 Refcount transition during split

When a super page is split, the base page's refcount (which tracked the whole
2 MB block) must be distributed to 512 individual refcounts. The correct
transition is:

- Before split: `page_refcount[base] = N`, `page_refcount[base+1..+511] = 0`
  (or sentinel).
- After split: `page_refcount[base] = N`, `page_refcount[base+1..+511] = N`.

If `N > 1` (COW shared), all 512 pages inherit the shared refcount. Each can
then be individually COW-faulted and freed.

### 6.5 Partial `munmap` / `mprotect` on a super page

If user code calls `munmap` on a VA range that covers only part of a super
page, we must split the super page first, then unmap the relevant 4 KB pages.
Same for `mprotect` on a sub-range. The `superpage_split()` function handles
this.

### 6.6 `kalloc_super()` fragmentation

After the system has been running, physical memory may be fragmented and
`kalloc_super()` may fail even though 2 MB of total memory is free. This is
acceptable -- callers must handle `NULL` return. A fallback to 512 individual
`kalloc()` calls is always possible (just use `mappages()` instead of
`mappages_super()`).

### 6.7 Kernel identity maps and the page allocator

Kernel identity maps (`0x80000000` to `PHYSTOP`) map physical memory that
includes the free list itself. These pages are not "allocated" via `kalloc()` --
the mapping just provides the kernel with access. The super page PTE for these
ranges does not consume the mapped memory; it only consumes the level-1 page
table itself (allocated via `kalloc()`). The refcounts for MMIO and kernel-text
pages in the identity map region are never manipulated (they are not in the
free list). No issue here.

### 6.8 `copy_pagetable_cow` skipping non-user pages

The existing code (line 283) skips pages without `PTE_U`. Kernel identity
mappings in per-process page tables do NOT have `PTE_U` set, so they are
correctly skipped. Super pages in kernel identity regions will never be
COW-shared; `sys_clone` re-creates them from scratch (lines 332-336). This is
safe.

---

## 7. Integration with Kernel Mapping Setup

### 7.1 Boot sequence

No changes to the boot sequence order. `kinit()` runs first, setting up the
4 KB free list. Then `kvminit()` sets up the kernel page table. The modified
`kvminit()` calls `mappages_super()` instead of `mappages()` for large aligned
regions. `mappages_super()` only needs to allocate level-1 page table pages
(via `kalloc()`), not the mapped memory.

Memory savings from super pages in kernel mapping:

Current approach with 4 KB pages for 128 MB kernel RAM:
- Level-2 table: 1 page (4 KB)
- Level-1 tables: 1 page (covers 1 GB VA range)
- Level-0 tables: 128 MB / 2 MB = 64 pages (64 * 4 KB = 256 KB)
- Total page-table overhead: ~260 KB

With 2 MB super pages for 128 MB kernel RAM:
- Level-2 table: 1 page (4 KB)
- Level-1 tables: 1 page (64 leaf entries for 128 MB)
- Level-0 tables: 0 (no level-0 tables needed)
- Total page-table overhead: ~8 KB

Savings: ~252 KB of page-table memory, plus improved TLB coverage (64 TLB
entries cover 128 MB instead of requiring 32768 entries for the same coverage).

### 7.2 PCIe/PLIC MMIO identity maps

These are large identity-mapped regions that benefit from super pages:
- PCIe ECAM (256 MB): 128 super pages instead of 65536 regular pages
- PCIe MMIO (1 GB): 512 super pages instead of 262144 regular pages
- PLIC (8 MB total): 4 super pages instead of 2048 regular pages

### 7.3 Per-process kernel mappings in `sys_clone()`

The `sys_clone()` function (trap.c lines 332-336) creates fresh identity
mappings in each child's page table. These should use `mappages_super()` for
the same regions as `kvminit()`.

---

## 8. Testing Approach

### 8.1 Unit-level verification (via `printk` + QEMU)

1. **`kalloc_super()` basic test:**
   Call `kalloc_super()` immediately after `kinit()`. Verify the returned
   address is 2 MB-aligned. Print the address. Call `kfree_super()` and verify
   the pages return to the free list (call `kalloc()` 512 times and check they
   succeed).

2. **`mappages_super()` basic test:**
   After `kvminit()`, read a known kernel address (e.g., `0x80200000` -- the
   kernel entry point). Use `walk_super()` to verify it returns a level-1 PTE.
   Print the level and the PTE value.

3. **`walk()` leaf detection test:**
   After super-page kernel mappings are established, call `walk()` on a VA
   within a 2 MB-mapped region. Verify it returns the level-1 leaf PTE (not
   a garbage level-0 PTE from interpreting data as a page table).

### 8.2 Kernel boot verification

1. Boot with super-page kernel mappings. If `kvminit()` succeeds and
   `printk` works after enabling paging, the super pages are functioning for
   kernel memory.
2. Verify UART output still works (UART is 4 KB mapped, should be unaffected).
3. Verify PLIC interrupts work (PLIC is now super-page mapped).

### 8.3 COW fork with super pages

1. Create a user process. Fork it (`sys_clone`). Verify the child runs.
2. In the child, write to a page. Verify the COW fault handler fires and the
   write succeeds.
3. Check that the parent's data is unchanged.
4. Since user processes use 4 KB pages and kernel identity maps use super pages
   (which are re-created, not COW-shared), the fork test primarily verifies
   that the modified `copy_pagetable_cow()` correctly skips non-user super
   pages and handles user 4 KB pages as before.

### 8.4 Super-page split test

1. Manually create a user-space 2 MB super page (for testing only).
2. Fork the process. Verify `copy_pagetable_cow()` splits the super page into
   512 individual COW pages.
3. Write to one of the 512 pages. Verify COW fault resolves correctly.
4. Write to another of the 512 pages. Verify independent COW resolution.

### 8.5 Stress / regression

1. Run the existing test programs (`spin`, `alarmtest`, `segvtest`) to verify
   no regressions.
2. Run a shell session: launch multiple processes, pipe data between them,
   verify everything works.
3. Fork bomb (limited): fork several times in succession, verify all children
   run and exit correctly.

### 8.6 TLB correctness

1. After splitting a super page, access all 512 sub-pages and verify correct
   data. This exercises TLB refill with the new 4 KB entries after the 2 MB
   entry is invalidated.

---

## 9. Implementation Order

Recommended order of implementation to allow incremental testing:

1. **Phase 1: Constants and macros** (riscv.h, kalloc.h, vm.h)
   - Add `SUPERPAGE_SIZE`, alignment macros, `PTE_IS_LEAF`, function
     declarations.
   - Build and verify no compilation errors.

2. **Phase 2: `walk()` modification**
   - Add leaf-PTE detection to `walk()`.
   - Add `walk_super()`.
   - Build and boot -- should behave identically since no super pages exist yet.

3. **Phase 3: `mappages_super()` and `kvmmap_super()`**
   - Implement `mappages_super()`.
   - Modify `kvminit()` to use super pages for kernel RAM.
   - Boot and verify kernel functions correctly.

4. **Phase 4: `uvmunmap()` and `uvmcopyin()` updates**
   - Handle super-page PTEs in unmapping and data copy.
   - Test with existing user programs.

5. **Phase 5: `kalloc_super()` and `kfree_super()`**
   - Implement the contiguous allocator.
   - Test allocation and deallocation.

6. **Phase 6: `superpage_split()` and COW changes**
   - Implement `superpage_split()`.
   - Modify `copy_pagetable_cow()` to split user super pages.
   - Test fork with existing programs.

7. **Phase 7: `sys_clone()` kernel map optimization**
   - Use `mappages_super()` for kernel identity maps in forked processes.
   - Full regression testing.

---

## 10. Files Changed (Summary)

| File | Lines affected | Nature of change |
|------|---------------|------------------|
| `kernel/include/riscv.h` | After line 25 | Add super-page macros, `PTE_IS_LEAF` |
| `kernel/include/kalloc.h` | After line 10 | Add `kalloc_super`, `kfree_super` declarations |
| `kernel/include/vm.h` | After line 36 | Add `mappages_super`, `walk_super`, `superpage_split`, `kvmmap_super` declarations |
| `kernel/kalloc.c` | After line 92 | New `kalloc_super()`, `kfree_super()` functions; guard in `kfree()` at line 44 |
| `kernel/vm.c` lines 14-27 | `walk()` | Add `PTE_IS_LEAF` check in loop body |
| `kernel/vm.c` after line 27 | New function | `walk_super()` |
| `kernel/vm.c` after line 51 | New function | `mappages_super()` |
| `kernel/vm.c` after line 59 | New function | `kvmmap_super()` |
| `kernel/vm.c` lines 62-90 | `kvminit()` | Use `kvmmap_super()` for large aligned regions |
| `kernel/vm.c` lines 133-145 | `uvmunmap()` | Handle super-page PTEs via `walk_super()` |
| `kernel/vm.c` after line 145 | New function | `superpage_split()` |
| `kernel/vm.c` lines 312-334 | `uvmcopyin()` | Handle offset within super pages |
| `kernel/trap.c` lines 276-303 | `copy_pagetable_cow()` | Detect and split super-page PTEs |
| `kernel/trap.c` lines 332-336 | `sys_clone()` | Use `mappages_super()` for kernel identity maps |
| `kernel/trap.c` lines 635-658 | COW fault handler | No change needed (split-on-fork strategy) |
