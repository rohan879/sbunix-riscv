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
#include <swap.h>
#include <pipe.h>
#include <kmalloc.h>
#include <syscall.h>

long sys_brk_handler(struct trapframe *tf) {
    uint64_t new_brk = tf->a0;
    uint64_t cur_sz  = current_proc->sz;

    if (new_brk == 0) {
        return (long)cur_sz;
    } else if (new_brk < cur_sz) {
        /* Shrink: unmap pages and remove VMA coverage */
        uint64_t new_rounded = PGROUNDUP(new_brk);
        uint64_t old_rounded = PGROUNDUP(cur_sz);
        if (new_rounded < old_rounded) {
            uvmunmap(current_proc->pagetable, new_rounded, old_rounded - new_rounded);
            vma_remove(current_proc, new_rounded, old_rounded);
            sfence_vma();
        }
        current_proc->sz = new_brk;
        return (long)new_brk;
    } else {
        /* Grow: lazy allocation -- just add a VMA, pages allocated on fault */
        uint64_t heap_start = PGROUNDUP(cur_sz);
        uint64_t rounded    = PGROUNDUP(new_brk);
        if (heap_start < rounded) {
            /* Add VMA for the new heap region; demand paging handles the rest */
            vma_add(current_proc, heap_start, rounded,
                    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON);
        }
        current_proc->sz = new_brk;
        return (long)new_brk;
    }
}

/* Forward declaration: used by MAP_FIXED in sys_mmap_handler */
/* RF: Made non-static for access from cleanup_proc_resources (exit path). */
void mmap_writeback_shared(struct proc *p, uint64_t start, uint64_t end);

/* BF: Compute the floor for mmap allocations, ensuring it never
 * falls below USER_MMAP_FLOOR (avoiding shared kernel MMIO range). */
static uint64_t mmap_search_base(struct proc *p) {
  uint64_t base = PGROUNDUP(p->sz) + PAGE_SIZE;
  if (base < USER_MMAP_FLOOR)
    base = USER_MMAP_FLOOR;
  return base;
}

/* BF: Find a hole in the user address space for a new mapping of len bytes.
 * Walks the VMA list to skip overlaps.  Returns 0 on failure (ENOMEM). */
static uint64_t mmap_find_hole(struct proc *p, uint64_t len) {
  uint64_t floor = mmap_search_base(p);
  uint64_t candidate = p->mmap_base ? p->mmap_base : floor;
  if (candidate < floor) candidate = floor;
  candidate = PGROUNDUP(candidate);
  for (;;) {
    if (candidate > USER_STACK_TOP || len > USER_STACK_TOP - candidate)
      return 0;
    struct vm_area *v = p->vma_list;
    int overlap = 0;
    while (v) {
      if (v->end <= candidate) { v = v->next; continue; }
      if (v->start >= candidate + len) break;
      candidate = PGROUNDUP(v->end);
      overlap = 1;
      break;
    }
    if (!overlap) return candidate;
  }
}

long sys_mmap_handler(struct trapframe *tf) {
    uint64_t addr  = tf->a0;
    uint64_t len   = tf->a1;
    int      prot  = (int)tf->a2;
    int      flags = (int)tf->a3;
    int      fd    = (int)tf->a4;
    uint64_t offset = tf->a5;

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
        int accmode = ftab[fd]->flags & 3; /* O_ACCMODE mask */
        if ((flags & 0x01 /* MAP_SHARED */) && (prot & PROT_WRITE)) {
            /* accmode 0 = O_RDONLY — not writable */
            if (accmode == 0)
                return (long)-13; /* EACCES */
        }
        if (prot & PROT_READ) {
            /* accmode 1 = O_WRONLY — not readable */
            if (accmode == 1)
                return (long)-13; /* EACCES */
        }
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
        /* Reject kernel addresses for MAP_FIXED */
        if (va >= 0x80000000UL || va + len > 0x80000000UL)
            return (long)-22; /* EINVAL */
        /* BF: Reject MAP_FIXED in shared MMIO range below mmap floor.
         * L1 entries for PLIC/UART are kernel superpages — user demand-paging
         * cannot service them. */
        if (va < USER_MMAP_FLOOR) {
            int level = -1;
            pte_t *mpte = walk_super(current_proc->pagetable, va, &level);
            if (mpte && (*mpte & PTE_V) && !(*mpte & PTE_U))
                return (long)-22; /* EINVAL */
        }
        /* BF: POSIX requires MAP_FIXED to replace any existing mappings
         * in the target range [va, va+len).  Without this, old PTEs and
         * VMA entries persist, causing incorrect data reads. */
        mmap_writeback_shared(current_proc, va, va + len);
        uvmunmap(current_proc->pagetable, va, len);
        sfence_vma();
        vma_remove(current_proc, va, va + len);
    } else {
        /* BF: Use mmap_find_hole() to find a gap that skips VMA overlaps
         * and stays above USER_MMAP_FLOOR (no kernel MMIO collision). */
        va = mmap_find_hole(current_proc, len);
        if (va == 0)
            return (long)-12; /* ENOMEM */
        current_proc->mmap_base = va + len;
    }

    int pte_flags = 0;
    if (prot & PROT_READ)  pte_flags |= PTE_R;
    if (prot & PROT_WRITE) pte_flags |= PTE_W;
    if (prot & PROT_EXEC)  pte_flags |= PTE_X;
    /* BF: Removed forced PTE_R for PROT_NONE -- demand paging handles this
     * correctly via the page fault handler which checks VMA prot flags
     * and delivers SIGSEGV for PROT_NONE access attempts. */

    /* Register VMA — pages allocated lazily on first access. */
    if (vma_add(current_proc, va, va + len, prot, flags) < 0) {
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
            ftab[fd]->refcount++; /* keep file open even if userspace closes fd */
        }
    }

    return (long)va;
}

/* Write back dirty MAP_SHARED pages to the backing file before unmapping.
 * RF: Made non-static for access from cleanup_proc_resources (exit path). */
void mmap_writeback_shared(struct proc *p, uint64_t start, uint64_t end) {
    struct vm_area *v = p->vma_list;
    while (v) {
        if (v->file && (v->flags & 0x01) /* MAP_SHARED */ &&
            v->start < end && v->end > start) {
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
                pte_t *pte = walk(p->pagetable, va, 0);
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

long sys_munmap_handler(struct trapframe *tf) {
    uint64_t addr = tf->a0;
    uint64_t len  = tf->a1;

    /* BF: POSIX requires EINVAL when len is 0.  Previously len=0 was
     * rounded up to 0 and silently accepted as a no-op. */
    if (len == 0)
        return -22; /* -EINVAL */

    /* BF: POSIX requires addr to be a multiple of page size.  Previously
     * non-aligned addresses were silently rounded down. */
    if (addr & (PAGE_SIZE - 1))
        return -22; /* -EINVAL */

    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    if (addr >= MAXVA || addr + len > MAXVA)
        return -22; /* -EINVAL */
    if (addr >= 0x80000000UL || addr + len > 0x80000000UL)
        return -22; /* -EINVAL: reject kernel addresses */

    /* Write back MAP_SHARED pages before unmapping */
    mmap_writeback_shared(current_proc, addr, addr + len);

    /* BF: Unmap pages BEFORE removing VMA tracking.  uvmunmap calls
     * page_remove_rmap (B7) which accesses page->anon_vma — the
     * anon_vma must still be alive.  If we did vma_remove first,
     * anon_vma_unlink could free the anon_vma, leaving page_remove_rmap
     * with a dangling pointer (use-after-free crash in swaptest). */
    uvmunmap(current_proc->pagetable, addr, len);
    sfence_vma();

    /* Remove VMA tracking (handles splits automatically).
     * BF: Check return — if split allocation fails the VMA list is
     * inconsistent, but pages are safely freed above. */
    vma_remove(current_proc, addr, addr + len);

    /* BF: Scan VMA list for highest non-stack allocated address.
     * Resets mmap_base to allow address space reuse after munmap.
     * Without this, 100k mmap/munmap cycles exhaust the address space
     * since mmap_base only moves forward.  Excludes the stack region
     * (which extends up to USER_STACK_TOP) from the scan so that
     * mmap_base does not get pushed to the ceiling of the address
     * space where mmap_find_hole would have no room to allocate. */
    {
        uint64_t highest = mmap_search_base(current_proc);
        struct vm_area *v = current_proc->vma_list;
        while (v) {
            /* BF: Skip the stack VMA — it reaches USER_STACK_TOP and
             * would push mmap_base to the address space ceiling. */
            if (v->end <= current_proc->stack_bottom ||
                v->start >= current_proc->stack_top) {
              /* Not a stack VMA — include in scan */
            } else {
              v = v->next;
              continue;
            }
            uint64_t end = v->end;
            if (end > highest)
                highest = end;
            v = v->next;
        }
        current_proc->mmap_base = highest;
    }

    return 0;
}

long sys_mprotect_handler(struct trapframe *tf) {
    uint64_t addr = tf->a0;
    uint64_t len  = tf->a1;
    int prot      = (int)tf->a2;

    /* Validate alignment */
    if (addr & (PAGE_SIZE - 1))
        return -22; /* -EINVAL */
    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (len == 0)
        return 0;
    if (addr + len > MAXVA)
        return -22;
    /* Reject kernel addresses — must not modify kernel page table entries */
    if (addr >= 0x80000000UL || addr + len > 0x80000000UL)
        return -22; /* -EINVAL */

    /* Build new PTE permission bits */
    int new_flags = 0;
    if (prot & PROT_READ)  new_flags |= PTE_R;
    if (prot & PROT_WRITE) new_flags |= PTE_W;
    if (prot & PROT_EXEC)  new_flags |= PTE_X;

    /* BF: Preflight allocation — count needed VMA splits and pre-allocate
     * all objects.  If any allocation fails, return -ENOMEM with no PTEs
     * or VMA list modified (failure atomicity). */
    struct vm_area *prealloc_vma[2] = {NULL, NULL};
    struct anon_vma_chain *prealloc_avc[2] = {NULL, NULL};
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
    /* Allocate VMA nodes and (if needed) anon_vma_chain nodes */
    for (int i = 0; i < n_splits && i < 2; i++) {
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
      /* Pre-allocate anon_vma_chain if source VMA has anon_vma —
       * we'll check this during mutation, but allocate speculatively
       * to avoid a second pass. */
      prealloc_avc[i] = (struct anon_vma_chain *)kmalloc(sizeof(struct anon_vma_chain));
      /* avc allocation failure is non-fatal — only needed if VMA has anon_vma */
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
        }

        if (!pte || !(*pte & PTE_V))
            continue;
        /* BF: Skip shared kernel MMIO PTEs — must not modify kernel entries */
        if (!(*pte & PTE_U))
            continue;

        /* BF: Preserve CoW state — do not re-enable PTE_W on a CoW page.
         * The write fault must still fire to perform the copy. */
        int pte_new_flags = new_flags;
        if (*pte & PTE_COW) {
          pte_new_flags &= ~PTE_W;
        }
        *pte = (*pte & ~(PTE_R | PTE_W | PTE_X)) | pte_new_flags;
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
        if (v->start < range_start && prealloc_idx < 2 && prealloc_vma[prealloc_idx]) {
          struct vm_area *suffix = prealloc_vma[prealloc_idx];
          struct anon_vma_chain *avc = prealloc_avc[prealloc_idx];
          prealloc_idx++;
          memset(suffix, 0, sizeof(*suffix));
          suffix->start = range_start;
          suffix->end   = v->end;
          suffix->prot  = v->prot;
          suffix->flags = v->flags;
          suffix->file  = v->file;
          suffix->file_offset = v->file_offset + (range_start - v->start);
          if (suffix->file) suffix->file->refcount++;
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
            if (avc) {
              anon_vma_link_prealloc(suffix, avc);
            } else {
              anon_vma_link(suffix);  /* fallback if avc alloc failed */
            }
          }
          suffix->next = v->next;
          v->end = range_start;
          v->next = suffix;
          v = suffix;
          continue;
        }

        /* Case 2: VMA ends after range — split off suffix */
        if (v->end > range_end && prealloc_idx < 2 && prealloc_vma[prealloc_idx]) {
          struct vm_area *tail = prealloc_vma[prealloc_idx];
          struct anon_vma_chain *avc = prealloc_avc[prealloc_idx];
          prealloc_idx++;
          memset(tail, 0, sizeof(*tail));
          tail->start = range_end;
          tail->end   = v->end;
          tail->prot  = v->prot;
          tail->flags = v->flags;
          tail->file  = v->file;
          tail->file_offset = v->file_offset + (range_end - v->start);
          if (tail->file) tail->file->refcount++;
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
            if (avc) {
              anon_vma_link_prealloc(tail, avc);
            } else {
              anon_vma_link(tail);  /* fallback if avc alloc failed */
            }
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
    for (int i = prealloc_idx; i < 2; i++) {
      if (prealloc_vma[i]) kmfree(prealloc_vma[i], sizeof(struct vm_area));
      if (prealloc_avc[i]) kmfree(prealloc_avc[i], sizeof(struct anon_vma_chain));
    }

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
    if (addr >= MAXVA || addr + len > MAXVA)
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
    old_size = PGROUNDUP(old_size);
    new_size = PGROUNDUP(new_size);
    if (new_size == old_size) return (long)old_addr;
    if (new_size < old_size) {
        /* BF: Shrink — writeback dirty MAP_SHARED pages before unmapping,
         * then use vma_remove() to properly trim/split VMAs instead of
         * the old v->end assignment which breaks after K5-style splits. */
        mmap_writeback_shared(current_proc, old_addr + new_size, old_addr + old_size);
        uvmunmap(current_proc->pagetable, old_addr + new_size, old_size - new_size);
        sfence_vma();
        vma_remove(current_proc, old_addr + new_size, old_addr + old_size);
        return (long)old_addr;
    }
    /* Grow: try to expand in place (check if space after is unmapped) */
    int can_expand = 1;
    for (uint64_t va = old_addr + old_size; va < old_addr + new_size; va += PAGE_SIZE) {
        struct vm_area *existing = vma_find(current_proc, va);
        /* BF: Also check for shared kernel MMIO PTEs blocking expansion */
        int level = -1;
        pte_t *epte = walk_super(current_proc->pagetable, va, &level);
        if (existing || (epte && (*epte & PTE_V) && !(*epte & PTE_U))) {
            can_expand = 0; break;
        }
    }
    if (can_expand) {
        struct vm_area *v = vma_find(current_proc, old_addr);
        if (v) v->end = old_addr + new_size;
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
        current_proc->mmap_base = new_va + new_size;

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
          /* BF: growth tail is always anonymous, never file-backed */
          if (vma_add(current_proc, new_va + old_size, new_va + new_size,
                      tail_prot, MAP_PRIVATE | MAP_ANON) < 0)
            goto mremap_rollback;
        }

        /* --- Step 4: Phase 1 -- copy pages without freeing old ones --- */
        for (uint64_t off = 0; off < old_size; off += PAGE_SIZE) {
          pte_t *old_pte = walk(current_proc->pagetable, old_addr + off, 0);
          if (!old_pte || *old_pte == 0) continue;

          struct vm_area *page_vma = vma_find(current_proc, new_va + off);
          int pte_flags = PTE_U;
          if (page_vma) {
            if (page_vma->prot & PROT_READ)  pte_flags |= PTE_R;
            if (page_vma->prot & PROT_WRITE) pte_flags |= PTE_W;
            if (page_vma->prot & PROT_EXEC)  pte_flags |= PTE_X;
          } else {
            pte_flags |= PTE_R | PTE_W;
          }

          if (*old_pte & PTE_V) {
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
          } else if (PTE_IS_SWAPPED(*old_pte)) {
            pte_t *new_pte = walk(current_proc->pagetable, new_va + off, 1);
            if (!new_pte) { copy_failed = 1; copied_up_to = off; break; }
            *new_pte = *old_pte;
          }
        }
        if (!copy_failed) goto mremap_phase2;

        /* --- Rollback: free only new pages; old mapping is INTACT --- */
        mremap_rollback:
        {
          for (uint64_t off = 0; off < copied_up_to; off += PAGE_SIZE) {
            pte_t *rpte = walk(current_proc->pagetable, new_va + off, 0);
            if (rpte && (*rpte & PTE_V)) {
              struct vm_area *rv = vma_find(current_proc, new_va + off);
              if (rv) {
                struct page *pd = virt_to_page((void *)PTE2PA(*rpte));
                if (pd) page_remove_rmap(pd, rv);
              }
              kfree((void *)PTE2PA(*rpte));
              *rpte = 0;
            } else if (rpte && PTE_IS_SWAPPED(*rpte)) {
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
            if (ov) {
              struct page *opd = virt_to_page((void *)pa);
              if (opd) page_remove_rmap(opd, ov);
            }
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
