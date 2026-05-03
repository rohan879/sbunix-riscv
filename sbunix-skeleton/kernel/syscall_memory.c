/* RF: Memory management syscall handlers extracted from trap.c.
 * mmap, munmap, mprotect, mremap, brk, madvise, msync. */

#include <syscall_handlers.h>
#include <trap_internal.h>
#include <pathres.h>
#include <proc.h>
#include <vm.h>
#include <mm.h>
#include <kalloc.h>
#include <vfs.h>
#include <ext2.h>
#include <riscv.h>
#include <string.h>
#include <printk.h>
#include <pipe.h>
#include <kmalloc.h>
#include <syscall.h>
#include <errno.h>
#include <shared_mapping.h>

static int mmap_reserved_range(struct proc *p, uint64_t start, uint64_t len,
                               uint64_t *next_va);

static int shared_vfork_child(void) {
    return current_proc && current_proc->shared_pt &&
           (current_proc->clone_flags & CLONE_VFORK);
}

static int brk_round_up(uint64_t value, uint64_t *rounded) {
    if (value > (uint64_t)-1 - (PAGE_SIZE - 1))
        return -1;
    *rounded = PGROUNDUP(value);
    if (*rounded < value)
        return -1;
    return 0;
}

static int ranges_overlap(uint64_t a_start, uint64_t a_end,
                          uint64_t b_start, uint64_t b_end) {
    return a_start < b_end && b_start < a_end;
}

static int mmap_prot_valid(int prot) {
    return (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) == 0;
}

static int mmap_flags_valid(int flags) {
    const int supported = MAP_SHARED | MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS;
    return (flags & ~supported) == 0;
}

static int brk_stack_guard_overlap(struct proc *p, uint64_t start,
                                   uint64_t end, uint64_t target) {
    if (!p || p->stack_top == 0 || p->stack_bottom == 0)
        return 0;

    uint64_t guard_bytes = 256 * PAGE_SIZE;
    uint64_t guard_start = p->stack_top > guard_bytes + PAGE_SIZE
                         ? p->stack_top - guard_bytes - PAGE_SIZE
                         : 0;
    uint64_t guard_end = p->stack_top ? p->stack_top : USER_STACK_TOP;

    return ranges_overlap(start, end, guard_start, guard_end) ||
           target >= guard_start;
}

static int brk_vma_overlap(struct proc *p, uint64_t start, uint64_t end) {
    struct vm_area *v = p->vma_list;

    while (v) {
        if (v->end <= start) {
            v = v->next;
            continue;
        }
        if (v->start >= end)
            break;
        return 1;
    }

    return 0;
}

static int brk_reserved_overlap(struct proc *p, uint64_t start, uint64_t end,
                                uint64_t target) {
    if (target >= SIGTRAMP_VA ||
        target >= USER_CLOCK_VA ||
        ranges_overlap(start, end, USER_CLOCK_VA, USER_CLOCK_VA + PAGE_SIZE) ||
        ranges_overlap(start, end, SIGTRAMP_VA, SIGTRAMP_VA + PAGE_SIZE))
        return 1;
    if (brk_stack_guard_overlap(p, start, end, target))
        return 1;
    if (brk_vma_overlap(p, start, end))
        return 1;
    if (mmap_reserved_range(p, start, end - start, 0))
        return 1;
    return 0;
}

long sys_brk_handler(struct trapframe *tf) {
    uint64_t new_brk = tf->a0;
    uint64_t cur_sz  = current_proc->sz;
    uint64_t new_rounded;
    uint64_t old_rounded;

    /* BF: A vfork child shares the parent's pagetable until exec/exit.
     * Reject VMA mutations in that window so the copied VMA list cannot
     * diverge from the shared physical mappings. */
    if (shared_vfork_child())
        return (long)cur_sz;

    if (new_brk == 0) {
        return (long)cur_sz;
    } else if (new_brk < current_proc->initial_brk) {
        /* BF: The loader's initial break is the lower heap boundary.
         * Shrinking below it would let brk overlap ELF data/BSS metadata,
         * so raw brk reports rejection by returning the unchanged break. */
        return (long)cur_sz;
    } else if (new_brk < cur_sz) {
        if (!user_addr_ok(new_brk) ||
            brk_round_up(new_brk, &new_rounded) < 0 ||
            brk_round_up(cur_sz, &old_rounded) < 0)
            return (long)cur_sz;

        /* BF: Keep raw brk failure Linux-like: reject invalid shrink targets
         * by returning the unchanged break instead of a negative errno. */
        if (new_rounded < old_rounded) {
            uvmunmap_proc(current_proc, current_proc->pagetable,
                         new_rounded, old_rounded - new_rounded);
            vma_remove(current_proc, new_rounded, old_rounded);
            sfence_vma();
        }
        current_proc->sz = new_brk;
        return (long)new_brk;
    } else {
        if (!user_addr_ok(new_brk) ||
            brk_round_up(cur_sz, &old_rounded) < 0 ||
            brk_round_up(new_brk, &new_rounded) < 0 ||
            !user_range_ok(old_rounded, new_rounded - old_rounded))
            return (long)cur_sz;

        /* Grow lazily, but first reject reserved holes and existing VMAs.
         * BF: vma_add() does not enforce non-overlap, so brk must preflight
         * the full heap extension before publishing a larger process size. */
        if (old_rounded < new_rounded) {
            if (brk_reserved_overlap(current_proc, old_rounded, new_rounded,
                                     new_brk))
                return (long)cur_sz;
            /* BF: brk expands a single heap frontier.  Extend the prior heap
             * VMA when possible to bound lazy-allocation metadata growth,
             * without enabling generic anonymous VMA merging elsewhere. */
            if (vma_extend_heap(current_proc, old_rounded, new_rounded,
                                PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANON) < 0)
                return (long)cur_sz;
        }
        current_proc->sz = new_brk;
        return (long)new_brk;
    }
}

/* Forward declaration: used by MAP_FIXED in sys_mmap_handler */
/* RF: Made non-static for access from cleanup_proc_resources (exit path). */
void mmap_writeback_shared(struct proc *p, uint64_t start, uint64_t end);

/* BF: Compute the floor for unhinted mmap allocations.  Low fixed hints are
 * still allowed; the default top-down search stays clear of the legacy device
 * aperture and early process image. */
static uint64_t mmap_search_base(struct proc *p) {
  uint64_t base = PGROUNDUP(p->sz) + PAGE_SIZE;
  if (base < USER_MMAP_FLOOR)
    base = USER_MMAP_FLOOR;
  return base;
}

static uint64_t mmap_search_limit(void) {
  /* BF: Unhinted mmap searches downward below the user clock/trampoline
   * pages.  Keeping large mappings high preserves room for later lazy heap
   * growth while leaving the fixed signal and clock pages reserved. */
  return PGROUNDDOWN(USER_CLOCK_VA);
}

/* BF: Treat kernel-owned aliases in a user page table as reserved holes.  User
 * page tables no longer copy low MMIO leaves, but this still protects fixed
 * kernel-owned pages such as the clock/trampoline and any supervisor mapping
 * inherited through future layout changes. */
static int mmap_reserved_range(struct proc *p, uint64_t start, uint64_t len,
                               uint64_t *next_va) {
  uint64_t end = start + len;

  if (len != 0 && end < start) {
    if (next_va)
      *next_va = USER_STACK_TOP;
    return 1;
  }

  /* BF: Keep libc's read-only CPU-clock page and signal trampoline out of
   * all user-controlled mapping operations. */
  if (ranges_overlap(start, end, USER_CLOCK_VA, USER_CLOCK_VA + PAGE_SIZE)) {
    if (next_va)
      *next_va = USER_CLOCK_VA + PAGE_SIZE;
    return 1;
  }
  if (ranges_overlap(start, end, SIGTRAMP_VA, SIGTRAMP_VA + PAGE_SIZE)) {
    if (next_va)
      *next_va = SIGTRAMP_VA + PAGE_SIZE;
    return 1;
  }

  for (uint64_t va = start; va < end;) {
    int level = -1;
    pte_t *pte = walk_super(p->pagetable, va, &level);
    uint64_t next = PGROUNDUP(va + 1);

    if (pte && (*pte & PTE_V) && level == 1)
      next = SPGROUNDUP(va + 1);
    if (next <= va || next > end)
      next = end;

    if (!pte || !(*pte & PTE_V) || (*pte & PTE_U)) {
      va = next;
      continue;
    }

    if (next_va) {
      if (level == 1)
        *next_va = SPGROUNDUP(va + 1);
      else
        *next_va = PGROUNDUP(va + 1);
    }
    return 1;
  }

  return 0;
}

/* BF: Find a top-down hole in the user address space for a new mapping of
 * len bytes.  Walks the VMA list to skip overlaps and starts above
 * USER_MMAP_FLOOR while still permitting callers to request low MAP_FIXED
 * addresses explicitly. */
static uint64_t mmap_find_hole(struct proc *p, uint64_t len) {
  uint64_t floor = mmap_search_base(p);
  uint64_t limit = mmap_search_limit();
  uint64_t candidate_end = limit;
  if (p->mmap_base > floor && p->mmap_base <= limit)
    candidate_end = PGROUNDDOWN(p->mmap_base);
  for (;;) {
    if (candidate_end <= floor || len > candidate_end - floor)
      return 0;
    uint64_t candidate = candidate_end - len;
    {
      uint64_t reserved_end = 0;
      if (mmap_reserved_range(p, candidate, len, &reserved_end)) {
        (void)reserved_end;
        candidate_end = candidate;
        continue;
      }
    }
    struct vm_area *v = p->vma_list;
    int overlap = 0;
    while (v) {
      if (v->end <= candidate) { v = v->next; continue; }
      if (v->start >= candidate_end) break;
      candidate_end = PGROUNDDOWN(v->start);
      overlap = 1;
      break;
    }
    if (!overlap) return candidate;
  }
}

static int mmap_file_max_prot(struct open_file *of, int flags) {
    int accmode = of ? (of->flags & 3) : 0;
    int max_prot = 0;

    /* BF: File-backed mappings always need read access for demand paging.
     * MAP_SHARED writes additionally require a writable fd. */
    if (accmode == 1) /* O_WRONLY */
        return 0;

    max_prot |= PROT_READ | PROT_EXEC;
    if (flags & MAP_PRIVATE)
        max_prot |= PROT_WRITE;
    if ((flags & 0x01) && accmode != 0)
        max_prot |= PROT_WRITE;
    return max_prot;
}

long sys_mmap_handler(struct trapframe *tf) {
    uint64_t addr  = tf->a0;
    uint64_t len   = tf->a1;
    int      prot  = (int)tf->a2;
    int      flags = (int)tf->a3;
    int      fd    = (int)tf->a4;
    uint64_t offset = tf->a5;
    struct shared_mapping *shared_obj = NULL;

    if (shared_vfork_child())
        return (long)-22; /* EINVAL */

    if (!mmap_prot_valid(prot) || !mmap_flags_valid(flags))
        return (long)-22; /* EINVAL */

    /* BF: POSIX requires offset to be page-aligned for mmap. */
    if (offset % PAGE_SIZE != 0)
        return (long)-22; /* EINVAL */

    /* BF: POSIX requires exactly one of MAP_SHARED or MAP_PRIVATE.
     * Return EINVAL if neither or both are set. */
    {
        int shared = flags & 0x01; /* MAP_SHARED */
        int priv   = flags & MAP_PRIVATE;
        if ((!shared && !priv) || (shared && priv))
            return (long)-22; /* EINVAL */
    }

    /* BF: MAP_FIXED requires a page-aligned address.  POSIX says
     * the implementation must not silently round down. */
    if ((flags & MAP_FIXED) && (addr & (PAGE_SIZE - 1)))
        return (long)-22; /* EINVAL */

    /* BF: Validate fd for non-anonymous (file-backed) mappings.
     * POSIX requires a valid open file descriptor. */
    struct open_file **ftab = proc_ofile(current_proc);
    if (!(flags & MAP_ANONYMOUS)) {
        if (fd < 0 || fd >= NOFILE || !ftab[fd])
            return (long)-9; /* EBADF */
        /* BF: Pipes are not memory-mappable.  POSIX requires ENODEV when
         * attempting to mmap a file descriptor that doesn't support it. */
        if (ftab[fd]->is_pipe)
            return (long)-19; /* ENODEV */
        /* BF: Check file permission vs requested prot.
         * MAP_SHARED + PROT_WRITE needs a writable fd.
         * PROT_READ needs a readable fd. */
        int max_prot = mmap_file_max_prot(ftab[fd], flags);
        if (max_prot == 0)
            return (long)-13; /* EACCES */
        if ((prot & ~max_prot) != 0)
            return (long)-13; /* EACCES */
    }

    /* BF: Check for overflow before rounding len up to page boundary.
     * ULONG_MAX + PAGE_SIZE-1 wraps to 0, which would pass later checks. */
    if (len > (uint64_t)-1 - (PAGE_SIZE - 1))
        return (long)-75; /* EOVERFLOW */

    /* BF: POSIX requires EOVERFLOW when off + len would overflow.
     * File-backed mappings with large offset can wrap around. */
    if (offset != 0 && offset + len < offset)
        return (long)-75; /* EOVERFLOW */

    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (len == 0) {
        return (long)-22; /* EINVAL: zero-length mapping */
    }

    uint64_t va;
    if (flags & MAP_FIXED) {
        va = addr; /* BF: addr already validated as page-aligned above */
        /* BF: Reject kernel addresses for MAP_FIXED.  Use subtraction form
         * (len > limit - va) to avoid unsigned wraparound when va + len
         * overflows uint64_t. */
        if (!user_range_ok(va, len))
            return (long)-22; /* EINVAL */
        if (mmap_reserved_range(current_proc, va, len, 0))
            return (long)-22; /* EINVAL */
        if (flags & MAP_SHARED) {
            int src = flags & MAP_ANONYMOUS
                    ? shared_mapping_create_anon(&shared_obj)
                    : shared_mapping_get_file(ftab[fd], &shared_obj);
            if (src < 0)
                return (long)src;
        }
        if (vma_remove_preflight(current_proc, va, va + len) < 0) {
            if (shared_obj)
                shared_mapping_put(shared_obj);
            return (long)-12; /* ENOMEM */
        }
        /* BF: POSIX requires MAP_FIXED to replace any existing mappings
         * in the target range [va, va+len).  Without this, old PTEs and
         * VMA entries persist, causing incorrect data reads. */
        mmap_writeback_shared(current_proc, va, va + len);
        uvmunmap_proc(current_proc, current_proc->pagetable, va, len);
        sfence_vma();
        vma_remove(current_proc, va, va + len);
    } else {
        /* BF: Use mmap_find_hole() to find a gap that skips VMA overlaps
         * and stays above USER_MMAP_FLOOR by default. */
        va = mmap_find_hole(current_proc, len);
        if (va == 0)
            return (long)-12; /* ENOMEM */
        if (flags & MAP_SHARED) {
            int src = flags & MAP_ANONYMOUS
                    ? shared_mapping_create_anon(&shared_obj)
                    : shared_mapping_get_file(ftab[fd], &shared_obj);
            if (src < 0)
                return (long)src;
        }
        current_proc->mmap_base = va;
    }

    /* BF: Removed forced PTE_R for PROT_NONE -- demand paging handles this
     * correctly via the page fault handler which checks VMA prot flags
     * and delivers SIGSEGV for PROT_NONE access attempts. */

    /* Register VMA — pages allocated lazily on first access. */
    if (vma_add(current_proc, va, va + len, prot, flags) < 0) {
        if (shared_obj)
            shared_mapping_put(shared_obj);
        /* BF: return -ENOMEM (not -1) so libc mmap wrapper sets
         * the correct errno for the caller. */
        return (long)-12; /* ENOMEM */
    }

    /* File-backed mapping: store file reference in VMA for demand paging.
     * ftab was already retrieved above during fd validation. */
    if (!(flags & MAP_ANONYMOUS) && fd >= 0 && fd < NOFILE && ftab[fd]) {
        struct vm_area *vma = vma_find(current_proc, va);
        if (vma) {
            vma->file = ftab[fd];
            vma->file_offset = offset;
            /* BF: Preserve the fd-derived maximum permission so later
             * mprotect() cannot upgrade a mapping beyond original access. */
            vma->max_prot = mmap_file_max_prot(ftab[fd], flags);
            ftab[fd]->refcount++; /* keep file open even if userspace closes fd */
        }
    }

    if (shared_obj) {
        struct vm_area *vma = vma_find(current_proc, va);
        if (!vma) {
            shared_mapping_put(shared_obj);
            vma_remove(current_proc, va, va + len);
            return (long)-12;
        }
        vma->shared = shared_obj;
        vma->shared_offset_pages = 0;
    }

    return (long)va;
}

/* Write back dirty MAP_SHARED pages to the backing file before unmapping.
 * BF: Accept an explicit pagetable and VMA list so exit/exec can flush the
 * old address space before its VMA metadata is torn down. */
void mmap_writeback_shared_from(pagetable_t pagetable, struct vm_area *vmas,
                                uint64_t start, uint64_t end) {
    struct vm_area *v = vmas;
    while (v) {
        if (v->file && (v->flags & 0x01) /* MAP_SHARED */ &&
            v->start < end && v->end > start) {
            if (v->shared) {
                shared_mapping_writeback_range(v, start, end);
                v = v->next;
                continue;
            }
            /* BF: Get file size ONCE per VMA to clip writeback per page.
             * Bytes beyond EOF in a partial page must NOT be persisted
             * (POSIX mmap partial-page zero-fill semantics). */
            /* BF: file_size = -1 means unknown (fstat absent or failed).
             * Changed from 0 to -1 to distinguish "empty file" from
             * "no fstat available".  Guard changed to >= 0 so zero-length
             * files correctly produce write_len=0 (POSIX: nothing to
             * write back for empty files). */
            int64_t file_size = -1;
            if (v->file->ops && v->file->ops->fstat) {
                uint8_t stbuf[128];
                memset(stbuf, 0, sizeof(stbuf));
                if (v->file->ops->fstat(v->file->fs_data, v->file->private,
                                    stbuf) == 0)
                    memcpy(&file_size, stbuf + 48, 8); /* st_size at offset 48 */
            }
            /* Writeback overlapping pages */
            uint64_t wb_start = (start > v->start) ? start : v->start;
            uint64_t wb_end = (end < v->end) ? end : v->end;
            for (uint64_t va = wb_start; va < wb_end; va += PAGE_SIZE) {
                pte_t *pte = walk(pagetable, va, 0);
                if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
                    uint64_t pa = PTE2PA(*pte);
                    uint64_t file_off = v->file_offset + (va - v->start);
                    int64_t foff = (int64_t)file_off;
                    /* BF: Limit write to actual file content -- do not
                     * persist the zero-fill tail beyond EOF. */
                    uint64_t write_len = PAGE_SIZE;
                    if (file_size >= 0 &&
                        file_off + PAGE_SIZE > (uint64_t)file_size) {
                        write_len = ((uint64_t)file_size > file_off)
                                        ? (uint64_t)file_size - file_off
                                        : 0;
                    }
                    if (write_len > 0 && v->file->ops && v->file->ops->write) {
                        v->file->ops->write(v->file->fs_data,
                                            v->file->private,
                                            (char *)pa, write_len, &foff);
                    }
                }
            }
        }
        v = v->next;
    }
}

/* RF: Process-scoped wrapper kept for mmap/munmap/msync callers. */
void mmap_writeback_shared(struct proc *p, uint64_t start, uint64_t end) {
    if (!p)
        return;
    mmap_writeback_shared_from(p->pagetable, p->vma_list, start, end);
}

long sys_munmap_handler(struct trapframe *tf) {
    uint64_t addr = tf->a0;
    uint64_t len  = tf->a1;

    if (shared_vfork_child())
        return -22; /* -EINVAL */

    /* BF: POSIX requires EINVAL when len is 0.  Previously len=0 was
     * rounded up to 0 and silently accepted as a no-op. */
    if (len == 0)
        return -22; /* -EINVAL */

    /* BF: POSIX requires addr to be a multiple of page size.  Previously
     * non-aligned addresses were silently rounded down. */
    if (addr & (PAGE_SIZE - 1))
        return -22; /* -EINVAL */

    /* BF: Guard against overflow before rounding len up to page boundary.
     * Without this, a huge len wraps around and passes subsequent checks. */
    if (len > (uint64_t)-1 - (PAGE_SIZE - 1))
        return -22; /* -EINVAL */
    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    if (!user_range_ok(addr, len))
        return -22; /* -EINVAL */
    if (mmap_reserved_range(current_proc, addr, len, 0))
        return -22; /* -EINVAL */
    if (vma_remove_preflight(current_proc, addr, addr + len) < 0)
        return -12; /* -ENOMEM */

    /* Write back MAP_SHARED pages before unmapping */
    mmap_writeback_shared(current_proc, addr, addr + len);

    /* BF: Unmap pages BEFORE removing VMA tracking.  PTE teardown owns the
     * kfree() call, but it still uses VMA/rmap metadata for advisory cleanup. */
    uvmunmap_proc(current_proc, current_proc->pagetable, addr, len);
    sfence_vma();

    /* Remove VMA tracking (handles splits automatically).
     * BF: Check return — if split allocation fails the VMA list is
     * inconsistent, but pages are safely freed above. */
    if (vma_remove(current_proc, addr, addr + len) < 0)
        return -12; /* -ENOMEM */

    /* BF: Reset the search hint after unmap so future allocations can
     * reuse low holes without inheriting stale stack-top or MMIO-adjacent
     * placement from older mappings. */
    current_proc->mmap_base = mmap_search_limit();

    return 0;
}

long sys_mprotect_handler(struct trapframe *tf) {
    uint64_t addr = tf->a0;
    uint64_t len  = tf->a1;
    int prot      = (int)tf->a2;

    if (shared_vfork_child())
        return -EINVAL;
    if (!mmap_prot_valid(prot))
        return -EINVAL;

    /* Validate alignment */
    if (addr & (PAGE_SIZE - 1))
        return -22; /* -EINVAL */
    /* BF: Guard against overflow before rounding len up to page boundary.
     * Without this, a huge len wraps around and passes subsequent checks. */
    if (len > (uint64_t)-1 - (PAGE_SIZE - 1))
        return -22; /* -EINVAL */
    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (len == 0)
        return 0;
    if (!user_range_ok(addr, len))
        return -22; /* -EINVAL */
    if (mmap_reserved_range(current_proc, addr, len, 0))
        return -EINVAL;

    for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE) {
        struct vm_area *v = vma_find(current_proc, va);
        if (!v)
            return -EINVAL;
        if ((prot & ~v->max_prot) != 0)
            return -EACCES;
    }

    /* Build new PTE permission bits */
    int new_flags = prot_to_pte_flags(prot);

    for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE) {
        int level;
        pte_t *pte = walk_super(current_proc->pagetable, va, &level);
        (void)level;
        if (pte && (*pte & PTE_V) && !(*pte & PTE_U))
            return -EINVAL;
    }

    /* BF: Preflight allocation — count needed VMA splits and pre-allocate
     * all objects.  If any allocation fails, return -ENOMEM with no PTEs
     * or VMA list modified (failure atomicity). */
    /* BF: Expanded preallocation from 2 to 8 to handle mprotect across
     * multiple VMAs.  Stack usage: 128 bytes (safe on 8KB stack). */
    #define MAX_MPROTECT_SPLITS 8
    struct vm_area *prealloc_vma[MAX_MPROTECT_SPLITS];
    struct anon_vma_chain *prealloc_avc[MAX_MPROTECT_SPLITS];
    for (int pi = 0; pi < MAX_MPROTECT_SPLITS; pi++) {
      prealloc_vma[pi] = NULL;
      prealloc_avc[pi] = NULL;
    }
    int n_splits = 0;
    {
      struct vm_area *v = current_proc->vma_list;
      while (v && v->start < addr + len) {
        if (v->end <= addr) { v = v->next; continue; }
        /* Prefix split needed if VMA starts before range */
        if (v->start < addr) n_splits++;
        /* Suffix split needed if VMA ends after range */
        if (v->end > addr + len) n_splits++;
        v = v->next;
      }
    }
    /* BF: Reject mprotect if more splits needed than preallocated cap.
     * Without this, excess splits are silently skipped, potentially
     * granting or removing permissions outside the requested range. */
    if (n_splits > MAX_MPROTECT_SPLITS)
      return -12; /* -ENOMEM */
    /* Allocate VMA nodes and (if needed) anon_vma_chain nodes */
    for (int i = 0; i < n_splits && i < MAX_MPROTECT_SPLITS; i++) {
      prealloc_vma[i] = (struct vm_area *)kmalloc(sizeof(struct vm_area));
      if (!prealloc_vma[i]) {
        /* BF: Allocation failed — free anything already allocated and
         * return ENOMEM.  No PTEs or VMAs have been touched. */
        for (int j = 0; j < i; j++) {
          kmfree(prealloc_vma[j], sizeof(struct vm_area));
          if (prealloc_avc[j])
            kmfree(prealloc_avc[j], sizeof(struct anon_vma_chain));
        }
        return -12;  /* -ENOMEM */
      }
      prealloc_avc[i] = (struct anon_vma_chain *)kmalloc(sizeof(struct anon_vma_chain));
      if (!prealloc_avc[i]) {
        /* BF: Split mutation must not allocate or fail after PTE updates.
         * Preallocate anon_vma_chain nodes for every possible split, even
         * if some are later freed unused for non-anonymous VMAs. */
        for (int j = 0; j <= i; j++) {
          if (prealloc_vma[j])
            kmfree(prealloc_vma[j], sizeof(struct vm_area));
          if (prealloc_avc[j])
            kmfree(prealloc_avc[j], sizeof(struct anon_vma_chain));
        }
        return -12;  /* -ENOMEM */
      }
    }
    int prealloc_idx = 0;

    /* Walk each page and update PTE flags */
    for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE) {
        int level;
        pte_t *pte = walk_super(current_proc->pagetable, va, &level);

        /* Split super page if needed */
        if (pte && (*pte & PTE_V) && level == 1) {
            superpage_split(current_proc->pagetable, va & ~(uint64_t)SUPERPAGE_MASK);
            pte = walk(current_proc->pagetable, va, 0);
            level = 0;
        }

        if (!pte || !(*pte & PTE_V))
            continue;
        if (!(*pte & PTE_U))
            continue;

        /* BF: Preserve CoW state — do not re-enable PTE_W on a CoW page.
         * The write fault must still fire to perform the copy. */
        int pte_new_flags = new_flags;
        if (*pte & PTE_COW) {
          pte_new_flags &= ~PTE_W;
        }
        if (pte_update(current_proc->pagetable, va, pte,
                       (*pte & ~(PTE_R | PTE_W | PTE_X)) | pte_new_flags,
                       level, PTE_UPDATE_MPROTECT,
                       PTE_SFENCE_NONE) < 0)
            return -EINVAL;
    }

    /* BF: Mutation phase — VMA splits using pre-allocated objects.
     * All allocations guaranteed available; no failure possible. */
    {
      uint64_t range_start = addr;
      uint64_t range_end = addr + len;
      struct vm_area *v = current_proc->vma_list;
      while (v && v->start < range_end) {
        if (v->end <= range_start) { v = v->next; continue; }

        /* Case 1: VMA starts before range — split off prefix */
        if (v->start < range_start && prealloc_idx < MAX_MPROTECT_SPLITS && prealloc_vma[prealloc_idx]) {
          struct vm_area *suffix = prealloc_vma[prealloc_idx];
          struct anon_vma_chain *avc = prealloc_avc[prealloc_idx];
          prealloc_idx++;
          memset(suffix, 0, sizeof(*suffix));
          suffix->start = range_start;
          suffix->end   = v->end;
          suffix->prot  = v->prot;
          suffix->max_prot = v->max_prot;
          suffix->flags = v->flags;
          suffix->file  = v->file;
          suffix->file_offset = v->file_offset + (range_start - v->start);
          if (suffix->file) suffix->file->refcount++;
          suffix->shared = v->shared;
          suffix->shared_offset_pages =
            v->shared_offset_pages + ((range_start - v->start) >> 12);
          if (suffix->shared)
            shared_mapping_get(suffix->shared);
          /* BF: Initialize anon_vma for split VMA using pre-allocated chain
           * to avoid allocation failure during mutation. */
          INIT_LIST_HEAD(&suffix->anon_vma_chain);
          suffix->anon_vma = NULL;
          suffix->vm_pgoff = v->vm_pgoff + ((range_start - v->start) >> 12);
          suffix->resident_pages = 0;
          if (v->anon_vma) {
            suffix->anon_vma = v->anon_vma;
            /* BF: Take a reference for the VMA's own anon_vma pointer.
             * anon_vma_link/_prealloc adds a separate reference for the
             * chain entry.  anon_vma_unlink does one put per chain plus
             * one put for vma->anon_vma, so we need both. */
            anon_vma_get(v->anon_vma);
            anon_vma_link_prealloc(suffix, avc);
          } else if (avc) {
            kmfree(avc, sizeof(struct anon_vma_chain));
            prealloc_avc[prealloc_idx - 1] = NULL;
          }
          suffix->next = v->next;
          v->end = range_start;
          v->next = suffix;
          v = suffix;
          continue;
        }

        /* Case 2: VMA ends after range — split off suffix */
        if (v->end > range_end && prealloc_idx < MAX_MPROTECT_SPLITS && prealloc_vma[prealloc_idx]) {
          struct vm_area *tail = prealloc_vma[prealloc_idx];
          struct anon_vma_chain *avc = prealloc_avc[prealloc_idx];
          prealloc_idx++;
          memset(tail, 0, sizeof(*tail));
          tail->start = range_end;
          tail->end   = v->end;
          tail->prot  = v->prot;
          tail->max_prot = v->max_prot;
          tail->flags = v->flags;
          tail->file  = v->file;
          tail->file_offset = v->file_offset + (range_end - v->start);
          if (tail->file) tail->file->refcount++;
          tail->shared = v->shared;
          tail->shared_offset_pages =
            v->shared_offset_pages + ((range_end - v->start) >> 12);
          if (tail->shared)
            shared_mapping_get(tail->shared);
          /* BF: Initialize anon_vma for split tail using pre-allocated chain. */
          INIT_LIST_HEAD(&tail->anon_vma_chain);
          tail->anon_vma = NULL;
          tail->vm_pgoff = v->vm_pgoff + ((range_end - v->start) >> 12);
          tail->resident_pages = 0;
          if (v->anon_vma) {
            tail->anon_vma = v->anon_vma;
            /* BF: Take a reference for the VMA's own anon_vma pointer.
             * Same rationale as prefix split above. */
            anon_vma_get(v->anon_vma);
            anon_vma_link_prealloc(tail, avc);
          } else if (avc) {
            kmfree(avc, sizeof(struct anon_vma_chain));
            prealloc_avc[prealloc_idx - 1] = NULL;
          }
          tail->next = v->next;
          v->end = range_end;
          v->next = tail;
        }

        /* Case 3: VMA is fully within range — update prot */
        v->prot = prot;
        v = v->next;
      }
    }

    /* Free unused pre-allocated objects */
    for (int i = prealloc_idx; i < MAX_MPROTECT_SPLITS; i++) {
      if (prealloc_vma[i]) kmfree(prealloc_vma[i], sizeof(struct vm_area));
      if (prealloc_avc[i]) kmfree(prealloc_avc[i], sizeof(struct anon_vma_chain));
    }

    #undef MAX_MPROTECT_SPLITS

    sfence_vma();
    return 0;
}

long sys_madvise_handler(struct trapframe *tf) {
    (void)tf;
    return (long)0;
}

/* msync — flush MAP_SHARED pages back to file */
long sys_msync_handler(struct trapframe *tf) {
    uint64_t addr = tf->a0 & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t len  = tf->a1;
    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (!user_range_ok(addr, len))
        return (long)-22; /* EINVAL */
    mmap_writeback_shared(current_proc, addr, addr + len);
    return 0;
}

/* mremap — grow or shrink an existing mapping */
long sys_mremap_handler(struct trapframe *tf) {
    uint64_t old_addr = tf->a0 & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t old_size = tf->a1;
    uint64_t new_size = tf->a2;
    int flags = (int)tf->a3;

    if (shared_vfork_child())
        return (long)-EINVAL;
    if (flags & ~1) /* supported: MREMAP_MAYMOVE */
        return (long)-EINVAL;
    old_size = PGROUNDUP(old_size);
    new_size = PGROUNDUP(new_size);
    if (!user_range_ok(old_addr, old_size) ||
        mmap_reserved_range(current_proc, old_addr, old_size, 0))
        return (long)-EINVAL;
    if (new_size == old_size) return (long)old_addr;
    if (new_size < old_size) {
        /* BF: Shrink — writeback dirty MAP_SHARED pages before unmapping,
         * then use vma_remove() to properly trim/split VMAs instead of
         * the old v->end assignment which breaks after K5-style splits. */
        if (vma_remove_preflight(current_proc, old_addr + new_size,
                                 old_addr + old_size) < 0)
            return (long)-12; /* ENOMEM */
        mmap_writeback_shared(current_proc, old_addr + new_size, old_addr + old_size);
        uvmunmap_proc(current_proc, current_proc->pagetable,
                     old_addr + new_size, old_size - new_size);
        sfence_vma();
        if (vma_remove(current_proc, old_addr + new_size,
                       old_addr + old_size) < 0)
            return (long)-12; /* ENOMEM */
        return (long)old_addr;
    }
    /* Grow: try to expand in place (check if space after is unmapped) */
    int can_expand = 1;
    for (uint64_t va = old_addr + old_size; va < old_addr + new_size; va += PAGE_SIZE) {
        struct vm_area *existing = vma_find(current_proc, va);
        if (existing || mmap_reserved_range(current_proc, va, PAGE_SIZE, 0)) {
            can_expand = 0; break;
        }
    }
    if (can_expand) {
        struct vm_area *v = vma_find(current_proc, old_addr);
        if (v && v->file && (v->flags & MAP_PRIVATE) &&
            old_addr + old_size == v->end) {
            /* BF: Growing a private file mapping past the original range
             * must not make the tail file-backed beyond EOF.  Keep the
             * existing VMA file-backed and publish an anonymous tail. */
            if (vma_add(current_proc, old_addr + old_size,
                        old_addr + new_size, v->prot,
                        MAP_PRIVATE | MAP_ANON) < 0)
                return (long)-12; /* ENOMEM */
        } else if (v) {
            v->end = old_addr + new_size;
        }
        return (long)old_addr;
    }
    /* Cannot expand in place; if MREMAP_MAYMOVE, allocate new region + copy */
    if (flags & 1) { /* MREMAP_MAYMOVE */
        /* BF: Full rewrite of MAYMOVE path with per-source-segment VMA
         * recreation, growth-tail policy, two-phase page copy, rollback
         * on failure, and proper anon_vma rmap registration.  The old
         * code used a single vma_add with hardcoded RW/PRIVATE|ANON
         * which lost file metadata, prot flags, and rmap chains. */
        uint64_t new_va = mmap_find_hole(current_proc, new_size);
        if (new_va == 0)
            return (long)-12; /* ENOMEM */
        current_proc->mmap_base = new_va;

        /* --- Step 1: Initialize rollback state --- */
        int copy_failed = 0;
        uint64_t copied_up_to = 0;

        /* --- Step 2: Recreate destination VMAs per source segment --- */
        {
          struct vm_area *sv = current_proc->vma_list;
          while (sv) {
            if (sv->start >= old_addr + old_size) break;
            if (sv->end <= old_addr) { sv = sv->next; continue; }
            uint64_t seg_start = (sv->start < old_addr) ? old_addr : sv->start;
            uint64_t seg_end = (sv->end > old_addr + old_size) ? old_addr + old_size : sv->end;
            uint64_t dst_start = new_va + (seg_start - old_addr);
            uint64_t dst_end = new_va + (seg_end - old_addr);

            if (vma_add(current_proc, dst_start, dst_end, sv->prot, sv->flags) < 0)
              goto mremap_rollback;

            /* BF: preserve file metadata from source VMA in destination */
            struct vm_area *dv = vma_find(current_proc, dst_start);
            if (dv && sv->file) {
              dv->file = sv->file;
              dv->file->refcount++;
              dv->file_offset = sv->file_offset + (seg_start - sv->start);
              dv->max_prot = sv->max_prot;
            }
            if (dv && sv->shared) {
              dv->shared = sv->shared;
              dv->shared_offset_pages =
                sv->shared_offset_pages + ((seg_start - sv->start) >> 12);
              shared_mapping_get(dv->shared);
            }
            sv = sv->next;
          }
        }

        /* --- Step 3: Growth tail (anonymous-only) --- */
        if (new_size > old_size) {
          struct vm_area *last_sv = NULL;
          struct vm_area *sv2 = current_proc->vma_list;
          while (sv2) {
            if (sv2->start >= old_addr + old_size) break;
            if (sv2->end > old_addr) last_sv = sv2;
            sv2 = sv2->next;
          }
          int tail_prot = last_sv ? last_sv->prot : (PROT_READ | PROT_WRITE);
          int tail_flags = last_sv && last_sv->shared
                         ? last_sv->flags
                         : (MAP_PRIVATE | MAP_ANON);
          if (vma_add(current_proc, new_va + old_size, new_va + new_size,
                      tail_prot, tail_flags) < 0)
            goto mremap_rollback;
          if (last_sv) {
            struct vm_area *tail_v = vma_find(current_proc, new_va + old_size);
            if (tail_v && last_sv->file && last_sv->shared) {
              tail_v->file = last_sv->file;
              tail_v->file->refcount++;
              tail_v->file_offset =
                last_sv->file_offset + ((old_addr + old_size) - last_sv->start);
              tail_v->max_prot = last_sv->max_prot;
            }
            if (tail_v && last_sv->shared) {
              tail_v->shared = last_sv->shared;
              tail_v->shared_offset_pages =
                last_sv->shared_offset_pages +
                (((old_addr + old_size) - last_sv->start) >> 12);
              shared_mapping_get(tail_v->shared);
            }
          }
        }

        /* --- Step 4: Phase 1 -- copy pages without freeing old ones --- */
        for (uint64_t off = 0; off < old_size; off += PAGE_SIZE) {
          pte_t *old_pte = walk(current_proc->pagetable, old_addr + off, 0);
          if (!old_pte || *old_pte == 0) continue;

          struct vm_area *page_vma = vma_find(current_proc, new_va + off);
          int pte_flags = PTE_U;
          if (page_vma) {
            pte_flags |= prot_to_pte_flags(page_vma->prot);
          } else {
            pte_flags |= PTE_R | PTE_W;
          }

          if (*old_pte & PTE_V) {
            if (page_vma && page_vma->shared) {
              uint64_t old_pa = PTE2PA(*old_pte);
              page_ref_inc(old_pa);
              if (mappages(current_proc->pagetable, new_va + off, PAGE_SIZE,
                           old_pa, pte_flags) != 0) {
                page_ref_dec(old_pa);
                copy_failed = 1; copied_up_to = off; break;
              }
              continue;
            }
            char *newpg = kalloc();
            if (!newpg) { copy_failed = 1; copied_up_to = off; break; }
            memcpy(newpg, (void *)PTE2PA(*old_pte), PAGE_SIZE);
            if (mappages(current_proc->pagetable, new_va + off, PAGE_SIZE,
                         (uint64_t)newpg, pte_flags) != 0) {
              kfree(newpg);
              copy_failed = 1; copied_up_to = off; break;
            }
            /* BF: register new page in rmap for reclaim */
            if (page_vma && page_vma->anon_vma) {
              struct page *pdesc = virt_to_page(newpg);
              if (pdesc) page_add_anon_rmap(pdesc, page_vma, new_va + off);
            }
          }
        }
        if (!copy_failed) goto mremap_phase2;

        /* --- Rollback: free only new pages; old mapping is INTACT --- */
        mremap_rollback:
        {
          for (uint64_t off = 0; off < copied_up_to; off += PAGE_SIZE) {
            pte_t *rpte = walk(current_proc->pagetable, new_va + off, 0);
            if (rpte && (*rpte & PTE_V)) {
              uint64_t pa = PTE2PA(*rpte);
              struct vm_area *rv = vma_find(current_proc, new_va + off);
              if (rv && (rv->flags & MAP_ANON)) {
                struct page *pd = virt_to_page((void *)pa);
                if (pd && PageAnon(pd))
                  page_remove_rmap(pd, rv);
              }
              /* BF: Rmap is metadata-only; every removed PTE drops exactly
               * one allocator ref through kfree(). */
              kfree((void *)pa);
              *rpte = 0;
            }
          }
          vma_remove(current_proc, new_va, new_va + new_size);
          return (long)-12; /* -ENOMEM */
        }

        /* --- Phase 2: Success -- clean up old mapping --- */
        mremap_phase2:
        for (uint64_t off = 0; off < old_size; off += PAGE_SIZE) {
          pte_t *old_pte = walk(current_proc->pagetable, old_addr + off, 0);
          if (!old_pte || *old_pte == 0) continue;
          if (*old_pte & PTE_V) {
            uint64_t pa = PTE2PA(*old_pte);
            struct vm_area *ov = vma_find(current_proc, old_addr + off);
            if (ov && (ov->flags & MAP_ANON)) {
              struct page *opd = virt_to_page((void *)pa);
              if (opd && PageAnon(opd))
                page_remove_rmap(opd, ov);
            }
            /* BF: PTE teardown owns physical release; rmap only updates
             * advisory metadata while the source VMA still exists. */
            kfree((void *)pa);
          }
          *old_pte = 0;
        }
        sfence_vma();
        vma_remove(current_proc, old_addr, old_addr + old_size);
        return (long)new_va;
    }
    return (long)-12; /* ENOMEM */
}
