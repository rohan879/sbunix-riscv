#include <kalloc.h>
#include <kmalloc.h>
#include <printk.h>
#include <riscv.h>
#include <string.h>
#include <vm.h>
#include <proc.h>
#include <swap.h>

pagetable_t kernel_pagetable;

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
// Detects leaf PTEs at level 1 (super pages) and returns them
// without descending further.
pte_t *walk(pagetable_t pagetable, uint64_t va, int alloc) {
    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pagetable[PX(level, va)];
        if (*pte & PTE_V) {
            if (PTE_IS_LEAF(*pte)) {
                /* This is a super page (level 1) or giga page (level 2).
                 * Cannot descend further -- return this leaf PTE. */
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

// Walk page table and return the PTE for va, plus the level at which
// it was found. Does NOT allocate intermediate page tables (lookup only).
// *level_out is set to 2, 1, or 0 for the level of the returned PTE,
// or -1 if no valid mapping exists.
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

    // Map the trampoline page at TRAMPOLINE_VA in kernel page table
    extern char _trampoline_start[];
    kvmmap(TRAMPOLINE_VA, (uint64_t)_trampoline_start, PAGE_SIZE,
           PTE_R | PTE_X);

    // Install the kernel page table (write to satp)
    w_satp(MAKE_SATP(kernel_pagetable));
    sfence_vma(); // flush TLB

    printk("kvminit: paging enabled (with 2MB super pages).\n");
}

/* Map the trampoline page and a process's trapframe page into a user
 * page table.  The trampoline is at TRAMPOLINE_VA (read+exec, no PTE_U
 * so only S-mode can execute it).  The trapframe is at TRAPFRAME_VA
 * (read+write, no PTE_U — accessed by the trampoline in S-mode). */
int uvmmap_trampoline(pagetable_t pt, struct trapframe *tf) {
  extern char _trampoline_start[];
  if (mappages(pt, TRAMPOLINE_VA, PAGE_SIZE,
               (uint64_t)_trampoline_start, PTE_R | PTE_X) < 0)
    return -1;
  if (mappages(pt, TRAPFRAME_VA, PAGE_SIZE,
               (uint64_t)tf, PTE_R | PTE_W) < 0)
    return -1;
  return 0;
}

// Create an empty user page table.
pagetable_t uvmcreate() {
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PAGE_SIZE);
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
            uint64_t pa = PTE2PA(*pte);
            if (level == 1) {
                /* Super page: free the 2MB block */
                kfree_super((void *)pa);
                *pte = 0;
                a += SUPERPAGE_SIZE;
            } else {
                /* Regular 4KB page */
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

  vma->start = start;
  vma->end   = end;
  vma->prot  = prot;
  vma->flags = flags;
  vma->next  = NULL;

  /* Insert into sorted list by start address */
  struct vm_area **pp = &p->vma_list;
  while (*pp && (*pp)->start < start)
    pp = &(*pp)->next;
  vma->next = *pp;
  *pp = vma;

  /* Merge with next if they touch and have same prot/flags */
  while (vma->next && vma->next->start <= vma->end &&
         vma->next->prot == vma->prot && vma->next->flags == vma->flags) {
    struct vm_area *nxt = vma->next;
    if (nxt->end > vma->end)
      vma->end = nxt->end;
    vma->next = nxt->next;
    kmfree(nxt, sizeof(struct vm_area));
  }

  /* Merge with previous if they touch and have same prot/flags */
  pp = &p->vma_list;
  while (*pp && *pp != vma) {
    struct vm_area *prev = *pp;
    if (prev->next == vma && prev->end >= vma->start &&
        prev->prot == vma->prot && prev->flags == vma->flags) {
      if (vma->end > prev->end)
        prev->end = vma->end;
      prev->next = vma->next;
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
      if (tail) {
        tail->start = end;
        tail->end   = v->end;
        tail->prot  = v->prot;
        tail->flags = v->flags;
        tail->next  = v->next;
        v->end = start;
        v->next = tail;
      }
      pp = &tail->next;
    } else if (v->start < start) {
      /* VMA starts before range: trim end */
      v->end = start;
      pp = &v->next;
    } else if (v->end > end) {
      /* VMA ends after range: trim start */
      v->start = end;
      pp = &v->next;
    } else {
      /* VMA fully within range: remove entirely */
      *pp = v->next;
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
    nv->start = sv->start;
    nv->end   = sv->end;
    nv->prot  = sv->prot;
    nv->flags = sv->flags;
    nv->next  = NULL;
    *dpp = nv;
    dpp = &nv->next;
    sv = sv->next;
  }
  return 0;
}

/* Free entire VMA list. Used by exec and exit. */
void vma_free_all(struct proc *p) {
  struct vm_area *v = p->vma_list;
  while (v) {
    struct vm_area *nxt = v->next;
    kmfree(v, sizeof(struct vm_area));
    v = nxt;
  }
  p->vma_list = NULL;
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
    } else if (!(pte & (PTE_R | PTE_W | PTE_X))) {
      /* Non-leaf PTE: points to next-level page table page */
      pagetable_t child = (pagetable_t)PTE2PA(pte);
      if (level > 0)
        freewalk_level(child, level - 1);
      kfree((void *)child);
      pt[i] = 0;
    } else if (pte & PTE_U) {
      /* User leaf PTE: free the physical page.
       * Skip kernel-only leaves (UART/CLINT/PLIC/PCIe I/O pages mapped
       * without PTE_U) — those are MMIO, not kalloc'd memory. */
      uint64_t pa = PTE2PA(pte);
      if (level == 1)
        kfree_super((void *)pa);
      else
        kfree((void *)pa);
      pt[i] = 0;
    }
    /* Kernel-only leaf entries (no PTE_U): leave them, they point to MMIO */
  }
}

void proc_freepagetable(pagetable_t pagetable) {
  if (!pagetable || pagetable == kernel_pagetable)
    return;
  /* User page tables no longer contain kernel identity mappings (Approach 4:
   * shared kernel page table with trap trampoline).  The only non-PTE_U
   * entries are the trampoline (PTE_R|PTE_X) and trapframe (PTE_R|PTE_W)
   * pages, which freewalk_level skips.  Walk the full root page table. */
  freewalk_level(pagetable, 2);
  /* Free the root page table page itself */
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
