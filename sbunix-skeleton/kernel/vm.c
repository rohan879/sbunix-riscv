#include <kalloc.h>
#include <kmalloc.h>
#include <printk.h>
#include <riscv.h>
#include <string.h>
#include <vm.h>
#include <proc.h>
#include <swap.h>
#include <mm.h>  /* BF: needed for anon_vma_get/link/unlink in vma_remove splits */

#ifndef VM_FREEWALK_TRACE
#define VM_FREEWALK_TRACE 1
#endif

#if VM_FREEWALK_TRACE
static int vm_freewalk_trace_count;
static void vm_trace_freewalk(const char *kind, pagetable_t pt, int level,
                              int index, pte_t pte, uint64_t pa) {
  if (vm_freewalk_trace_count >= 128)
    return;
  printk("freewalk[%s] pt=0x%lx lvl=%d idx=%d pte=0x%lx pa=0x%lx ref=%d\n",
         kind, (uint64_t)pt, level, index, pte, pa, page_ref_get(pa));
  vm_freewalk_trace_count++;
}
#else
#define vm_trace_freewalk(kind, pt, level, index, pte, pa) ((void)0)
#endif

pagetable_t kernel_pagetable;

static int pa_is_valid_page(uint64_t pa);
static int pa_is_valid_superpage(uint64_t pa);
static int pa_is_live_page(uint64_t pa);
static int pa_is_live_superpage(uint64_t pa);
static int pa_is_live_ptpage(uint64_t pa);

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
// Detects leaf PTEs at level 1 (super pages) and returns them
// without descending further.
pte_t *walk(pagetable_t pagetable, uint64_t va, int alloc) {
    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pagetable[PX(level, va)];
        if (*pte & PTE_V) {
            if (PTE_IS_LEAF(*pte) || (*pte & PTE_U)) {
                /* This is a super page (level 1) or giga page (level 2).
                 * Cannot descend further -- return this leaf PTE. */
                return pte;
            }
            uint64_t child_pa = PTE2PA(*pte);
            if (!pa_is_valid_page(child_pa)) {
                return 0;
            }
            pagetable = (pagetable_t)child_pa;
        } else {
            if (!alloc || (pagetable = (pagetable_t)kalloc()) == 0)
                return 0;
            memset(pagetable, 0, PAGE_SIZE);
            page_mark_ptpage((uint64_t)pagetable);  /* Track as page table page */
            *pte = PA2PTE(pagetable) | PTE_V;
        }
    }
    return &pagetable[PX(0, va)];
}

// Walk page table and return the PTE for va, plus the level at which
// it was found. Does NOT allocate intermediate page tables (lookup only).
// *level_out is set to 2, 1, or 0 for the level of the returned PTE,
// or -1 if no valid mapping exists.
pte_t *walk_super(pagetable_t pagetable, uint64_t va, int *level_out) {
    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pagetable[PX(level, va)];
        if (*pte & PTE_V) {
            if (PTE_IS_LEAF(*pte) || (*pte & PTE_U)) {
                if (level_out) *level_out = level;
                return pte;
            }
            uint64_t child_pa = PTE2PA(*pte);
            if (!pa_is_valid_page(child_pa)) {
                if (level_out) *level_out = -1;
                return 0;
            }
            pagetable = (pagetable_t)child_pa;
        } else {
            if (level_out) *level_out = -1;
            return 0;
        }
    }
    pte_t *pte = &pagetable[PX(0, va)];
    if (level_out) *level_out = 0;
    return pte;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
int mappages(pagetable_t pagetable, uint64_t va, uint64_t size, uint64_t pa, int perm) {
    uint64_t a, last;
    pte_t *pte;

    if (size == 0) return 0;

    a = PGROUNDDOWN(va);
    last = PGROUNDDOWN(va + size - 1);
    for(;;) {
        if((pte = walk(pagetable, a, 1)) == 0)
            return -1;
        if(*pte & PTE_V) {
            // Already mapped, but we continue
        }
        *pte = PA2PTE(pa) | perm | PTE_V;
        if(a == last)
            break;
        a += PAGE_SIZE;
        pa += PAGE_SIZE;
    }
    return 0;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(uint64_t va, uint64_t pa, uint64_t sz, int perm) {
    if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
        printk("kvmmap failed\n");
}

// Create level-1 leaf PTEs (2MB super page mappings).
// va, pa must be 2MB-aligned; size must be a multiple of 2MB.
int mappages_super(pagetable_t pagetable, uint64_t va, uint64_t size,
                   uint64_t pa, int perm) {
    if (!IS_SUPERPAGE_ALIGNED(va) || !IS_SUPERPAGE_ALIGNED(pa) ||
        !IS_SUPERPAGE_ALIGNED(size))
        return -1;

    uint64_t end = va + size;
    for (; va < end; va += SUPERPAGE_SIZE, pa += SUPERPAGE_SIZE) {
        /* Walk to level 2, install level-1 PTE ourselves */
        pte_t *l2pte = &pagetable[PX(2, va)];
        pagetable_t l1pt;
        if (*l2pte & PTE_V) {
            l1pt = (pagetable_t)PTE2PA(*l2pte);
        } else {
            l1pt = (pagetable_t)kalloc();
            if (!l1pt) return -1;
            memset(l1pt, 0, PAGE_SIZE);
            page_mark_ptpage((uint64_t)l1pt);  /* Track as page table page */
            *l2pte = PA2PTE(l1pt) | PTE_V;
        }

        pte_t *l1pte = &l1pt[PX(1, va)];
        /* Install leaf PTE at level 1 */
        *l1pte = PA2PTE(pa) | perm | PTE_V;
    }
    return 0;
}

// Add a 2MB super page mapping to the kernel page table.
void kvmmap_super(uint64_t va, uint64_t pa, uint64_t sz, int perm) {
    if (mappages_super(kernel_pagetable, va, sz, pa, perm) != 0)
        printk("kvmmap_super failed\n");
}

// Initialize the one kernel_pagetable.
// Uses 2MB super pages for large aligned identity-mapped regions.
void kvminit(void) {
    kernel_pagetable = (pagetable_t)kalloc();
    memset(kernel_pagetable, 0, PAGE_SIZE);
    page_mark_ptpage((uint64_t)kernel_pagetable);  /* Track kernel root page table */

    // UART registers: too small for super page, use 4KB
    kvmmap(0x10000000, 0x10000000, PAGE_SIZE, PTE_R | PTE_W);

    // Goldfish RTC at 0x101000 (one 4KB page)
    kvmmap(0x101000, 0x101000, PAGE_SIZE, PTE_R | PTE_W);

    // PCIe ECAM config space (0x30000000, 254 MiB) -- 2MB-aligned
    // Reduced by 2MB to free VPN[1]=511 for TRAMPOLINE_VA mapping
    // (the last 2MB at 0x3FE00000-0x3FFFFFFF conflicts with TRAMPOLINE_VA's
    // level-1 page table index, and super page leaf PTEs prevent 4KB entries)
    kvmmap_super(0x30000000, 0x30000000, 0x0FE00000, PTE_R | PTE_W);

    // PCIe MMIO BAR window (0x40000000 - 0x80000000, 1 GiB) -- 2MB-aligned
    kvmmap_super(0x40000000, 0x40000000, 0x40000000, PTE_R | PTE_W);

    // PLIC: 4MB each, 2MB-aligned
    kvmmap_super(0x0c000000, 0x0c000000, 0x400000, PTE_R | PTE_W);
    kvmmap_super(0x0c200000, 0x0c200000, 0x400000, PTE_R | PTE_W);

    // Kernel RAM: 128MB from 0x80000000, 2MB-aligned -- use super pages
    kvmmap_super(0x80000000, 0x80000000, PHYSTOP - 0x80000000,
                 PTE_R | PTE_W | PTE_X);

    /* Sv39 self-referencing page table: install a 1GB leaf gigapage
     * at L2[510] that maps physical RAM [0x80000000, 0xC0000000) to
     * a dedicated VA window.  This provides a redundant access path
     * to page table pages that is independent of the L1 superpages
     * (which are the PTEs being corrupted by DMA).
     *
     * Leaf gigapage: PTE_R | PTE_W makes this a leaf at L2 level.
     * No PTE_X (page table data should not be executable).
     * No PTE_U (kernel-only access). */
    kernel_pagetable[RECURSIVE_INDEX] =
        PA2PTE(0x80000000UL) | PTE_V | PTE_R | PTE_W;

    // Install the kernel page table (write to satp)
    w_satp(MAKE_SATP(kernel_pagetable));
    sfence_vma(); // flush TLB

    printk("kvminit: paging enabled (with 2MB super pages).\n");
}

/* Check and repair the kernel code L1 PTE via the self-referencing
 * gigapage window.  Returns 1 if repair was needed, 0 if PTE was
 * correct.  Does NOT call walk(). */
int recursive_check_kcode_pte(void) {
  volatile pte_t *ppte = recursive_l1_pte(0x80200000UL);
  if (!ppte) return 0;
  pte_t pte = *ppte;
  if ((pte & PTE_V) && !(pte & PTE_X)) {
    *ppte = pte | PTE_X;
    asm volatile("sfence.vma zero, zero" ::: "memory");
    return 1;
  }
  if (!(pte & PTE_V)) {
    *ppte = PA2PTE(0x80200000UL) | PTE_V | PTE_R | PTE_W | PTE_X;
    asm volatile("sfence.vma zero, zero" ::: "memory");
    return 1;
  }
  return 0;
}

/* Scan all 64 kernel RAM L1 PTEs (0x80000000-0x87FFFFFF) via the
 * self-referencing window.  Fix any that lost PTE_X. */
int recursive_check_all_kernel_ptes(void) {
  int repairs = 0;
  for (int i = 0; i < 64; i++) {
    uint64_t va = 0x80000000UL + (uint64_t)i * SUPERPAGE_SIZE;
    volatile pte_t *ppte = recursive_l1_pte(va);
    if (!ppte) continue;
    pte_t pte = *ppte;
    if ((pte & PTE_V) && !(pte & PTE_X)) {
      *ppte = pte | PTE_X;
      repairs++;
    }
  }
  if (repairs > 0)
    asm volatile("sfence.vma zero, zero" ::: "memory");
  return repairs;
}

/* Map kernel page table entries into a user page table by sharing
 * references (not copying page table pages).  This avoids the ~65
 * page table page leak that occurred when kernel entries were
 * duplicated per-process. */
int uvmmap_kernel(pagetable_t pt) {
  /* Share VPN[2]=1..511: these root entries cover kernel RAM, PCIe MMIO BAR,
   * and other regions entirely above user space. */
  for (int i = 1; i < 512; i++)
    pt[i] = kernel_pagetable[i];

  /* VPN[2]=0: user and kernel MMIO share this region.  The user already
   * has its own level-1 table (created by mappages for code/stack).
   * Copy kernel MMIO level-1 entries into empty slots of that table. */
  pte_t kpte0 = kernel_pagetable[0];
  pte_t upte0 = pt[0];
  if (!(kpte0 & PTE_V) || !(upte0 & PTE_V))
    return 0; /* nothing to share */

  pagetable_t kernel_L1 = (pagetable_t)PTE2PA(kpte0);
  pagetable_t user_L1   = (pagetable_t)PTE2PA(upte0);
  /* BF: Only share kernel MMIO L1 entries below USER_MMAP_FLOOR.
   * L1 index j maps VA range [j*2MB, (j+1)*2MB) within VPN[2]=0.
   * L1[128] = UART at 0x10000000, safely below mmap floor.
   * L1[384-510] = PCIe ECAM at 0x30000000+ — within user mmap range, SKIP.
   * Limit = USER_MMAP_FLOOR / SUPERPAGE_SIZE = 0x10200000 / 0x200000 = 129. */
  int share_limit = USER_MMAP_FLOOR / SUPERPAGE_SIZE;
  for (int j = 0; j < share_limit; j++) {
    if ((kernel_L1[j] & PTE_V) && !(user_L1[j] & PTE_V))
      user_L1[j] = kernel_L1[j];
  }
  return 0;
}

// Create an empty user page table.
pagetable_t uvmcreate() {
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PAGE_SIZE);
  page_mark_ptpage((uint64_t)pagetable);  /* Track as user root page table */
  return pagetable;
}

// Allocate PTEs and physical memory for a virtual address range [start_va, end_va).
// Returns end_va or 0 on error.
uint64_t uvmalloc(pagetable_t pagetable, uint64_t start_va, uint64_t end_va, int perm) {
  char *mem;
  uint64_t a;

  if(end_va < start_va)
    return start_va;

  start_va = PGROUNDDOWN(start_va);
  for(a = start_va; a < end_va; a += PAGE_SIZE){
    mem = kalloc();
    if(mem == 0){
      return 0;
    }
    memset(mem, 0, PAGE_SIZE);
    if(mappages(pagetable, a, PAGE_SIZE, (uint64_t)mem, perm|PTE_U|PTE_V) != 0){
      kfree(mem);
      return 0;
    }
  }
  return end_va;
}

// Map a user virtual address to a physical address
int uvmmap(pagetable_t pagetable, uint64_t va, uint64_t pa, uint64_t sz, int perm) {
    return mappages(pagetable, va, sz, pa, perm | PTE_U);
}

// Unmap and free physical pages for a user virtual address range [va, va+len).
// va and len must be page-aligned.  Silently skips pages that are not mapped.
// Handles both 4KB pages and 2MB super pages.
void uvmunmap(pagetable_t pagetable, uint64_t va, uint64_t len) {
    uint64_t a = va;
    uint64_t end = va + len;

    while (a < end) {
        int level;
        pte_t *pte = walk_super(pagetable, a, &level);
        if (!pte) {
            a += PAGE_SIZE;
            continue;
        }
        if (*pte & PTE_V) {
            /* BF: Skip kernel-only PTEs (shared MMIO via uvmmap_kernel) —
             * must not free kernel pages from user unmap paths. */
            if (!(*pte & PTE_U)) {
                a += (level == 1) ? SUPERPAGE_SIZE : PAGE_SIZE;
                continue;
            }
            uint64_t pa = PTE2PA(*pte);
            if (level == 1) {
                /* Super page: free the 2MB block */
                kfree_super((void *)pa);
                *pte = 0;
                a += SUPERPAGE_SIZE;
            } else {
                /* Regular 4KB page.
                 * BF: B7 (page_remove_rmap) intentionally omitted.
                 * Without a matching page_add_mapping in copy_pagetable_cow
                 * (reverted due to exit-path mapcount leak), calling
                 * page_remove_rmap here would drive mapcount below -1.
                 * Stale page descriptor state is cleaned up in kalloc()
                 * when the page is next allocated. */
                kfree((void *)pa);
                *pte = 0;
                a += PAGE_SIZE;
            }
        } else if (PTE_IS_SWAPPED(*pte)) {
            /* Swapped-out page: free the swap slot */
            swap_free(PTE_SWAP_SLOT(*pte));
            *pte = 0;
            a += PAGE_SIZE;
        } else {
            /* Completely unmapped */
            a += PAGE_SIZE;
        }
    }
}

// Demote a 2MB super page at va into 512 individual 4KB PTEs.
// va must be 2MB-aligned. If the PTE at va is not a level-1 leaf,
// this function does nothing.  Each constituent 4KB page gets its
// refcount set to 1 via page_ref_set (the sub-pages previously had
// no individual refcount tracking).
void superpage_split(pagetable_t pagetable, uint64_t va) {
    int level;
    pte_t *pte = walk_super(pagetable, va, &level);
    if (!pte || level != 1 || !(*pte & PTE_V))
        return; /* not a super page */

    uint64_t pa = PTE2PA(*pte);
    int flags = PTE_FLAGS(*pte);

    /* Clear the super page PTE */
    *pte = 0;

    /* Create 512 level-0 PTEs */
    for (int i = 0; i < SUPERPAGE_NPAGES; i++) {
        uint64_t sub_va = va + (uint64_t)i * PAGE_SIZE;
        uint64_t sub_pa = pa + (uint64_t)i * PAGE_SIZE;
        /* Set refcount for each sub-page to 1 via page_ref_set */
        page_ref_set(sub_pa, 1);
        mappages(pagetable, sub_va, PAGE_SIZE, sub_pa, flags);
    }
    sfence_vma();
}

/* ------------------------------------------------------------------ */
/* VMA management functions                                             */
/* ------------------------------------------------------------------ */

/* Add a VMA covering [start, end) with given prot/flags.
 * Inserts into the sorted linked list, merging with adjacent VMAs
 * that have matching prot and flags. Returns 0 on success, -1 on error. */
int vma_add(struct proc *p, uint64_t start, uint64_t end, int prot, int flags) {
  if (start >= end) return -1;

  struct vm_area *vma = (struct vm_area *)kmalloc(sizeof(struct vm_area));
  if (!vma) return -1;
  memset(vma, 0, sizeof(*vma));

  vma->start = start;
  vma->end   = end;
  vma->prot  = prot;
  vma->flags = flags;
  vma->next  = NULL;
  vma->file  = NULL;
  vma->file_offset = 0;

  /* Initialize anon_vma fields */
  INIT_LIST_HEAD(&vma->anon_vma_chain);
  vma->anon_vma = NULL;
  vma->vm_pgoff = 0;
  vma->resident_pages = 0;
  
  /* For anonymous mappings, create anon_vma if needed */
  if (flags & MAP_ANON) {
    vma->anon_vma = anon_vma_alloc();
    if (!vma->anon_vma) {
      kmfree(vma, sizeof(struct vm_area));
      return -1;
    }
    if (anon_vma_link(vma) < 0) {
      anon_vma_put(vma->anon_vma);
      kmfree(vma, sizeof(struct vm_area));
      return -1;
    }
  }

  /* Insert into sorted list by start address */
  struct vm_area **pp = &p->vma_list;
  while (*pp && (*pp)->start < start)
    pp = &(*pp)->next;
  vma->next = *pp;
  *pp = vma;

  /* Merge with next if they touch and have same prot/flags */
  while (vma->next && vma->next->start <= vma->end &&
         vma->next->prot == vma->prot && vma->next->flags == vma->flags &&
         (vma->flags & MAP_ANON) && (vma->next->flags & MAP_ANON) &&
         vma->file == NULL && vma->next->file == NULL) {
    struct vm_area *nxt = vma->next;
    if (nxt->end > vma->end)
      vma->end = nxt->end;
    vma->next = nxt->next;
    /* Unlink nxt from anon_vma chains before freeing */
    if (nxt->anon_vma) {
      anon_vma_unlink(nxt);
    }
    kmfree(nxt, sizeof(struct vm_area));
  }

  /* Merge with previous if they touch and have same prot/flags */
  pp = &p->vma_list;
  while (*pp && *pp != vma) {
    struct vm_area *prev = *pp;
    if (prev->next == vma && prev->end >= vma->start &&
        prev->prot == vma->prot && prev->flags == vma->flags &&
        (prev->flags & MAP_ANON) && (vma->flags & MAP_ANON) &&
        prev->file == NULL && vma->file == NULL) {
      if (vma->end > prev->end)
        prev->end = vma->end;
      prev->next = vma->next;
      /* Unlink vma from anon_vma chains before freeing */
      if (vma->anon_vma) {
        anon_vma_unlink(vma);
      }
      kmfree(vma, sizeof(struct vm_area));
      break;
    }
    pp = &(*pp)->next;
  }

  return 0;
}

/* Find the VMA containing addr (start <= addr < end), or NULL. */
struct vm_area *vma_find(struct proc *p, uint64_t addr) {
  struct vm_area *v = p->vma_list;
  while (v) {
    if (addr >= v->start && addr < v->end)
      return v;
    v = v->next;
  }
  return NULL;
}

/* Remove VMAs overlapping [start, end). Handle partial overlaps by
 * splitting. Free removed nodes with kmfree(). Returns 0. */
int vma_remove(struct proc *p, uint64_t start, uint64_t end) {
  struct vm_area **pp = &p->vma_list;
  while (*pp) {
    struct vm_area *v = *pp;

    /* No overlap: VMA is entirely before the range */
    if (v->end <= start) {
      pp = &v->next;
      continue;
    }
    /* No overlap: VMA is entirely after the range */
    if (v->start >= end)
      break;

    /* Some overlap exists */
    if (v->start < start && v->end > end) {
      /* VMA spans both sides of the range: split into two */
      struct vm_area *tail = (struct vm_area *)kmalloc(sizeof(struct vm_area));
      if (!tail)
        return -1;
      memset(tail, 0, sizeof(*tail));
      uint64_t old_start = v->start;
      tail->start = end;
      tail->end   = v->end;
      tail->prot  = v->prot;
      tail->flags = v->flags;
      tail->next  = v->next;
      tail->file = v->file;
      tail->file_offset = v->file_offset + (end - old_start);
      if (tail->file)
        tail->file->refcount++;
      /* BF: Initialize anon_vma fields on split tail so that rmap
       * chains remain consistent and page reclaim doesn't follow
       * uninitialized pointers. */
      INIT_LIST_HEAD(&tail->anon_vma_chain);
      tail->anon_vma = NULL;
      tail->vm_pgoff = v->vm_pgoff + ((end - v->start) >> 12);
      tail->resident_pages = 0;
      /* BF: anon_vma_link() internally calls anon_vma_get(), so we must
       * NOT call anon_vma_get() separately — that would double-increment
       * the refcount and leak the anon_vma on VMA removal. */
      if (v->anon_vma) {
        tail->anon_vma = v->anon_vma;
        anon_vma_link(tail);
      }
      v->end = start;
      v->next = tail;
      pp = &tail->next;
    } else if (v->start < start) {
      /* VMA starts before range: trim end */
      v->end = start;
      pp = &v->next;
    } else if (v->end > end) {
      /* VMA ends after range: trim start */
      if (v->file)
        v->file_offset += (end - v->start);
      v->start = end;
      pp = &v->next;
    } else {
      /* VMA fully within range: remove entirely */
      *pp = v->next;
      /* BF: Unlink from anon_vma chains before freeing to prevent
       * dangling rmap entries that would corrupt page reclaim. */
      if (v->anon_vma) {
        anon_vma_unlink(v);
      }
      if (v->file) {
        vfs_free_file(v->file);
        v->file = NULL;
      }
      kmfree(v, sizeof(struct vm_area));
    }
  }
  return 0;
}

/* Deep copy the VMA list from src to dst process. Used by fork.
 * Returns 0 on success, -1 on error. */
int vma_dup(struct proc *src, struct proc *dst) {
  dst->vma_list = NULL;
  struct vm_area **dpp = &dst->vma_list;
  struct vm_area *sv = src->vma_list;
  while (sv) {
    struct vm_area *nv = (struct vm_area *)kmalloc(sizeof(struct vm_area));
    if (!nv) {
      vma_free_all(dst);
      return -1;
    }
    memset(nv, 0, sizeof(*nv));
    nv->start = sv->start;
    nv->end   = sv->end;
    nv->prot  = sv->prot;
    nv->flags = sv->flags;
    nv->file = sv->file;
    nv->file_offset = sv->file_offset;
    if (nv->file)
      nv->file->refcount++;
    nv->next  = NULL;
    
    /* Initialize anon_vma fields for child VMA */
    INIT_LIST_HEAD(&nv->anon_vma_chain);
    nv->anon_vma = NULL;
    nv->vm_pgoff = sv->vm_pgoff;
    nv->resident_pages = 0;  /* Child starts with no resident pages */
    
    /* Set up anon_vma inheritance for anonymous mappings */
    if (sv->flags & MAP_ANON && sv->anon_vma) {
      if (anon_vma_fork(nv, sv) < 0) {
        kmfree(nv, sizeof(struct vm_area));
        vma_free_all(dst);
        return -1;
      }
    }
    *dpp = nv;
    dpp = &nv->next;
    sv = sv->next;
  }
  return 0;
}

/* Helper to check if a pointer looks like a valid kmalloc'd VMA address */
static int vma_ptr_valid(struct vm_area *v) {
  uint64_t addr = (uint64_t)v;
  /* Must be within kernel physical RAM and reasonably aligned (at least 8-byte) */
  return addr >= 0x80000000UL && addr < PHYSTOP && (addr & 7) == 0;
}

/* Free entire VMA list. Used by exec and exit. */
void vma_free_all(struct proc *p) {
  struct vm_area *v = p->vma_list;
  int count = 0;
  while (v) {
    /* Sanity check for corrupted pointer */
    if (!vma_ptr_valid(v)) {
      printk("vma_free_all: corrupted vma ptr 0x%lx at count=%d, aborting\n",
             (uint64_t)v, count);
      break;
    }
    struct vm_area *nxt = v->next;
    /* Release file reference for file-backed mappings */
    if (v->file) {
      vfs_free_file(v->file);
      v->file = NULL;
    }
    
    /* Unlink from anon_vma chains for anonymous mappings */
    if (v->flags & MAP_ANON && v->anon_vma) {
      anon_vma_unlink(v);
    }
    
    kmfree(v, sizeof(struct vm_area));
    v = nxt;
    count++;
    /* Safety limit to prevent infinite loops on corrupted lists */
    if (count > 10000) {
      printk("vma_free_all: too many VMAs (%d), possible corruption\n", count);
      break;
    }
  }
  p->vma_list = NULL;
}

static int pa_is_valid_page(uint64_t pa) {
  return pa != 0 &&
         (pa % PAGE_SIZE) == 0 &&
         pa >= 0x80000000UL &&
         pa < PHYSTOP;
}

static int pa_is_valid_superpage(uint64_t pa) {
  return pa_is_valid_page(pa) &&
         IS_SUPERPAGE_ALIGNED(pa) &&
         pa + SUPERPAGE_SIZE <= PHYSTOP;
}

static int pa_is_live_page(uint64_t pa) {
  if (!pa_is_valid_page(pa))
    return 0;
  /* Only consider it a live page if it has non-zero refcount.
   * This prevents double-freeing pages that were already freed. */
  return page_ref_get(pa) > 0;
}

static int pa_is_live_superpage(uint64_t pa) {
  if (!pa_is_valid_superpage(pa))
    return 0;
  /* Only consider it live if base page has non-zero refcount */
  return page_ref_get(pa) > 0;
}

static int pa_is_live_ptpage(uint64_t pa) {
  if (!pa_is_valid_page(pa))
    return 0;
  /* Only consider it a live page table page if:
   * 1. It has non-zero refcount (allocated)
   * 2. It was actually marked as a page table page at allocation time
   * This prevents treating reused data pages as page tables. */
  return page_ref_get(pa) > 0 && page_is_ptpage(pa);
}

/* Recursively free page-table pages and user leaf pages in a sub-tree.
 * Kernel-only leaf entries (no PTE_U) are skipped so MMIO and kernel
 * RAM are never kfree'd.  Page-table pages themselves are always freed.
 * Leaf pages are freed via kfree (which decrements refcount for COW). */
static void freewalk_level(pagetable_t pt, int level) {
  for (int i = 0; i < 512; i++) {
    pte_t pte = pt[i];
    if (!(pte & PTE_V) && !PTE_IS_SWAPPED(pte))
      continue;
    if (PTE_IS_SWAPPED(pte)) {
      swap_free(PTE_SWAP_SLOT(pte));
      pt[i] = 0;
    } else if (pte & PTE_U) {
      /* User leaf PTEs can legally have no R/W/X bits (for example after
       * mprotect(PROT_NONE)). Treat them as leaves before the non-leaf case. */
      uint64_t pa = PTE2PA(pte);
      if (level == 1) {
        if (pa_is_live_superpage(pa)) {
          vm_trace_freewalk("leaf-super", pt, level, i, pte, pa);
          kfree_super((void *)pa);
        }
      } else if (pa_is_live_page(pa)) {
        vm_trace_freewalk("leaf-page", pt, level, i, pte, pa);
        kfree((void *)pa);
      }
      pt[i] = 0;
    } else if (!(pte & (PTE_R | PTE_W | PTE_X))) {
      if (level == 0) {
        pt[i] = 0;
        continue;
      }
       /* Non-leaf PTE: points to next-level page table page.
         * Guard against corrupted page table pointers (e.g., PPN==0 or
         * values outside physical RAM). If the child pagetable PA is
         * invalid we log and detach the entry rather than attempting
         * to recurse into it and crash the kernel. */
      pagetable_t child = (pagetable_t)PTE2PA(pte);
      if (!pa_is_live_ptpage((uint64_t)child)) {
        /* Corrupted/reused page table page — silently detach */
        pt[i] = 0;
        continue;
      }
      /* Clear PTE BEFORE recursing to prevent double-free if this same
       * physical page appears elsewhere in the page table tree (circular
       * reference or duplicate PTE). This ensures we only free each page
       * table page once even if the tree structure is corrupted. */
      pt[i] = 0;
      if (level > 0)
        freewalk_level(child, level - 1);
      vm_trace_freewalk("pt-child", pt, level, i, pte, (uint64_t)child);
      kfree((void *)child);
    }
    /* Kernel-only leaf entries (no PTE_U): leave them, they point to MMIO */
  }
}

void proc_freepagetable(pagetable_t pagetable) {
  if (!pagetable || pagetable == kernel_pagetable)
    return;

  /* VPN[2]=1..511: shared kernel entries — detach without freeing */
  for (int i = 1; i < 512; i++)
    pagetable[i] = 0;

  /* VPN[2]=0: user owns this level-1 table, but some entries are
   * shared kernel MMIO.  Compare against kernel's level-1 to identify. */
  pte_t kpte0 = kernel_pagetable[0];
  pte_t upte0 = pagetable[0];
  if ((upte0 & PTE_V) && !(upte0 & (PTE_R | PTE_W | PTE_X))) {
    pagetable_t user_L1 = (pagetable_t)PTE2PA(upte0);
    pagetable_t kernel_L1 = (kpte0 & PTE_V) ? (pagetable_t)PTE2PA(kpte0) : 0;
    /* BF: Limit shared MMIO detach to same range as uvmmap_kernel() share limit.
     * Entries above share_limit were never shared, so no need to compare. */
    int share_limit = USER_MMAP_FLOOR / SUPERPAGE_SIZE;
    for (int j = 0; j < 512; j++) {
      if (j < share_limit && kernel_L1 && user_L1[j] == kernel_L1[j]) {
        user_L1[j] = 0; /* shared MMIO — detach */
      } else if (user_L1[j] & PTE_V) {
        /* User-owned entry — free recursively. A user leaf may have no R/W/X
         * bits after mprotect(PROT_NONE), so handle PTE_U before non-leaf. */
        if (user_L1[j] & PTE_U) {
          uint64_t pa = PTE2PA(user_L1[j]);
          if (pa_is_live_superpage(pa)) {
            vm_trace_freewalk("l1-super", user_L1, 1, j, user_L1[j], pa);
            kfree_super((void *)pa);
          }
        } else if (!(user_L1[j] & (PTE_R | PTE_W | PTE_X))) {
          /* Non-leaf: recurse into level-0 table */
          pagetable_t child = (pagetable_t)PTE2PA(user_L1[j]);
          pte_t saved_pte = user_L1[j];  /* save for trace after clearing */
          if (pa_is_live_ptpage((uint64_t)child)) {
            /* Clear PTE before recursion to prevent double-free */
            user_L1[j] = 0;
            freewalk_level(child, 0);
            vm_trace_freewalk("l1-child", user_L1, 1, j, saved_pte, (uint64_t)child);
            kfree((void *)child);
          } else {
            /* Corrupted/reused L0 page — silently detach */
            user_L1[j] = 0;
          }
        } else {
          user_L1[j] = 0;
        }
      } else if (PTE_IS_SWAPPED(user_L1[j])) {
        swap_free(PTE_SWAP_SLOT(user_L1[j]));
        user_L1[j] = 0;
      }
    }
    vm_trace_freewalk("user-l1-root", pagetable, 2, 0, upte0, (uint64_t)user_L1);
    kfree((void *)user_L1);
    pagetable[0] = 0;
  }

  /* Free the root page table page itself */
  vm_trace_freewalk("pt-root", pagetable, 2, -1, 0, (uint64_t)pagetable);
  kfree((void *)pagetable);
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
// Handles both 4KB pages and 2MB super pages.
int copyout(pagetable_t pagetable, uint64_t dstva, void *src, uint64_t len) {
  char *s = (char *)src;
  while (len > 0) {
    if (dstva >= MAXVA)
      return -1;
    int level;
    pte_t *pte = walk_super(pagetable, dstva, &level);
    if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
      return -1;

    /* Resolve COW page before writing.  Also handle the case where
     * PTE_COW was lost (e.g., after a swap round-trip) but the page
     * is still shared (ref > 1): must copy before writing. */
    if (level == 0) {
      uint64_t old_pa = PTE2PA(*pte);
      int ref = page_ref_get(old_pa);
      if ((*pte & PTE_COW) || (ref > 1 && !(*pte & PTE_W))) {
        int flags = PTE_FLAGS(*pte);
        if (ref > 1) {
          /* Shared: allocate new page and copy */
          char *newpg = kalloc();
          if (!newpg) return -1;
          memcpy(newpg, (void *)old_pa, PAGE_SIZE);
          page_ref_dec(old_pa);
          *pte = PA2PTE((uint64_t)newpg) | (flags & ~PTE_COW) | PTE_W;
        } else {
          /* Last reference: just remove COW and add write */
          *pte = (*pte & ~PTE_COW) | PTE_W;
        }
        sfence_vma();
      }
      if ((*pte & PTE_W) == 0)
        return -1;
    } else {
      if ((*pte & PTE_W) == 0)
        return -1;
    }

    uint64_t page_size = (level == 1) ? SUPERPAGE_SIZE : PAGE_SIZE;
    uint64_t va0 = (level == 1) ? SPGROUNDDOWN(dstva) : PGROUNDDOWN(dstva);
    uint64_t pa0 = PTE2PA(*pte) + (dstva - va0);
    uint64_t n = page_size - (dstva - va0);
    if (n > len)
      n = len;

    // Copy to physical address directly since we are in kernel 1:1 map
    memcpy((void *)pa0, s, n);

    len -= n;
    s += n;
    dstva = va0 + page_size;
  }
  return 0;
}

// Copy from user virtual address srcva to kernel buffer dst.
// Returns 0 on success, -1 on error (unmapped/invalid user page).
int copyin(pagetable_t pagetable, void *dst, uint64_t srcva, uint64_t len) {
  char *d = (char *)dst;
  while (len > 0) {
    if (srcva >= MAXVA)
      return -1;
    int level;
    pte_t *pte = walk_super(pagetable, srcva, &level);
    if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
      return -1;

    uint64_t page_size = (level == 1) ? SUPERPAGE_SIZE : PAGE_SIZE;
    uint64_t va0 = (level == 1) ? SPGROUNDDOWN(srcva) : PGROUNDDOWN(srcva);
    uint64_t pa0 = PTE2PA(*pte) + (srcva - va0);
    uint64_t n = page_size - (srcva - va0);
    if (n > len)
      n = len;

    memcpy(d, (void *)pa0, n);

    len -= n;
    d += n;
    srcva = va0 + page_size;
  }
  return 0;
}

// Copy null-terminated string from user virtual address srcva to kernel buffer dst.
// Copies at most max bytes (including the null). Returns 0 if null found, -1 otherwise.
int copyinstr(pagetable_t pagetable, char *dst, uint64_t srcva, uint64_t max) {
  int got_null = 0;
  while (max > 0) {
    if (srcva >= MAXVA)
      return -1;
    int level;
    pte_t *pte = walk_super(pagetable, srcva, &level);
    if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
      return -1;

    uint64_t page_size = (level == 1) ? SUPERPAGE_SIZE : PAGE_SIZE;
    uint64_t va0 = (level == 1) ? SPGROUNDDOWN(srcva) : PGROUNDDOWN(srcva);
    uint64_t pa0 = PTE2PA(*pte) + (srcva - va0);
    uint64_t n = page_size - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)pa0;
    for (uint64_t i = 0; i < n; i++) {
      *dst = p[i];
      if (p[i] == '\0') {
        got_null = 1;
        break;
      }
      dst++;
    }
    if (got_null)
      break;

    max -= n;
    srcva = va0 + page_size;
  }
  return got_null ? 0 : -1;
}
