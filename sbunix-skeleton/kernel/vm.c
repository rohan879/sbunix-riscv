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

    // PCIe ECAM config space (0x30000000, 256 MiB) -- 2MB-aligned
    kvmmap_super(0x30000000, 0x30000000, 0x10000000, PTE_R | PTE_W);

    // PCIe MMIO BAR window (0x40000000 - 0x80000000, 1 GiB) -- 2MB-aligned
    kvmmap_super(0x40000000, 0x40000000, 0x40000000, PTE_R | PTE_W);

    // PLIC: 4MB each, 2MB-aligned
    kvmmap_super(0x0c000000, 0x0c000000, 0x400000, PTE_R | PTE_W);
    kvmmap_super(0x0c200000, 0x0c200000, 0x400000, PTE_R | PTE_W);

    // Kernel RAM: 128MB from 0x80000000, 2MB-aligned -- use super pages
    kvmmap_super(0x80000000, 0x80000000, PHYSTOP - 0x80000000,
                 PTE_R | PTE_W | PTE_X);

    // Install the kernel page table (write to satp)
    w_satp(MAKE_SATP(kernel_pagetable));
    sfence_vma(); // flush TLB

    printk("kvminit: paging enabled (with 2MB super pages).\n");
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
            int slot = pte_swap_take(pte, 0);
            if (slot >= 0)
                swap_free(slot);
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

// Map the signal trampoline page into a user page table.
extern char sigtrampoline[];
extern char sigtrampoline_end[];

void uvmmap_sigtrampoline(pagetable_t pagetable) {
  char *pg = kalloc();
  if (!pg) return;
  memset(pg, 0, PAGE_SIZE);
  uint64_t sz = (uint64_t)(sigtrampoline_end - sigtrampoline);
  if (sz > PAGE_SIZE) sz = PAGE_SIZE;
  memcpy(pg, sigtrampoline, sz);
  mappages(pagetable, SIG_TRAMPOLINE_VA, PAGE_SIZE, (uint64_t)pg,
           PTE_R | PTE_X | PTE_U);
}

/* ------------------------------------------------------------------ */
/* VMA management functions                                             */
/* ------------------------------------------------------------------ */

/* Add a VMA covering [start, end) with given prot/flags.
 * Inserts into the sorted linked list, merging with adjacent VMAs
 * that have matching prot and flags. Returns 0 on success, -1 on error. */
int vma_add_list(struct vm_area **head, uint64_t start, uint64_t end, int prot,
                 int flags) {
  if (start >= end) return -1;

  struct vm_area *prev = NULL;
  struct vm_area **pp = head;
  while (*pp && (*pp)->start < start) {
    prev = *pp;
    pp = &(*pp)->next;
  }

  /* Merge directly into the previous compatible VMA when possible. */
  if (prev && prev->end >= start &&
      prev->prot == prot && prev->flags == flags) {
    if (end > prev->end)
      prev->end = end;

    while (prev->next && prev->next->start <= prev->end &&
           prev->next->prot == prot && prev->next->flags == flags) {
      struct vm_area *nxt = prev->next;
      if (nxt->end > prev->end)
        prev->end = nxt->end;
      prev->next = nxt->next;
      kmfree(nxt, sizeof(struct vm_area));
    }
    return 0;
  }

  /* Likewise extend the next compatible VMA backward if it touches us. */
  if (*pp && (*pp)->start <= end &&
      (*pp)->prot == prot && (*pp)->flags == flags) {
    struct vm_area *vma = *pp;
    if (start < vma->start)
      vma->start = start;
    if (end > vma->end)
      vma->end = end;

    while (vma->next && vma->next->start <= vma->end &&
           vma->next->prot == prot && vma->next->flags == flags) {
      struct vm_area *nxt = vma->next;
      if (nxt->end > vma->end)
        vma->end = nxt->end;
      vma->next = nxt->next;
      kmfree(nxt, sizeof(struct vm_area));
    }
    return 0;
  }

  struct vm_area *vma = (struct vm_area *)kmalloc(sizeof(struct vm_area));
  if (!vma) return -1;

  vma->start = start;
  vma->end   = end;
  vma->prot  = prot;
  vma->flags = flags;
  vma->next  = *pp;
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

  return 0;
}

int vma_add(struct proc *p, uint64_t start, uint64_t end, int prot, int flags) {
  return vma_add_list(&p->vma_list, start, end, prot, flags);
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
void vma_free_list(struct vm_area **head) {
  struct vm_area *v = *head;
  while (v) {
    struct vm_area *nxt = v->next;
    kmfree(v, sizeof(struct vm_area));
    v = nxt;
  }
  *head = NULL;
}

void vma_free_all(struct proc *p) {
  vma_free_list(&p->vma_list);
}

/* ------------------------------------------------------------------ */
/* Free a user-process page table, releasing only user pages (PTE_U). */
/* Kernel identity-mapped leaf PTEs are left alone.                    */
/* ------------------------------------------------------------------ */
/* Returns 1 if pa is within kalloc-managed RAM, 0 otherwise. */
static int is_ram_pa(uint64_t pa) {
    return pa >= 0x80000000UL && pa < PHYSTOP;
}

static void _freewalk_user(pagetable_t pt, int level, uint64_t *seen_slots) {
    for (int i = 0; i < 512; i++) {
        pte_t pte = pt[i];
        if (pte == 0)
            continue;
        if (!(pte & PTE_V)) {
            /* Not valid — may be a swapped page */
            if (level == 0 && PTE_IS_SWAPPED(pte)) {
                int slot = pte_swap_take(&pt[i], 0);
                if (slot >= 0) {
                    int w = slot / 64;
                    uint64_t bit = 1ULL << (slot % 64);
                    if ((seen_slots[w] & bit) == 0) {
                        seen_slots[w] |= bit;
                        swap_free(slot);
                    }
                }
            } else {
                pt[i] = 0;
            }
            continue;
        }
        uint64_t pa = PTE2PA(pte);
        if (PTE_IS_LEAF(pte)) {
            /* Only free physical pages belonging to userspace in RAM */
            if ((pte & PTE_U) && is_ram_pa(pa)) {
                if (level == 1)
                    kfree_super((void *)pa);
                else
                    kfree((void *)pa);
            }
        } else if (level > 0) {
            if (is_ram_pa(pa)) {
                _freewalk_user((pagetable_t)pa, level - 1, seen_slots);
                kfree((void *)pa);
            }
        }
        pt[i] = 0;
    }
}

void freewalk_user(pagetable_t pagetable) {
    uint64_t seen_slots[(SWAP_NSLOTS + 63) / 64];
    memset(seen_slots, 0, sizeof(seen_slots));
    _freewalk_user(pagetable, 2, seen_slots);
    kfree((void *)pagetable);
}

/* Update PTE permissions for a range. Only affects valid mapped PTEs. */
void uvmprotect(pagetable_t pagetable, uint64_t va, uint64_t len, int perm) {
    uint64_t a = PGROUNDDOWN(va);
    uint64_t end = PGROUNDUP(va + len);
    for (; a < end; a += PAGE_SIZE) {
        pte_t *pte = walk(pagetable, a, 0);
        if (pte && (*pte & PTE_V))
            *pte = PA2PTE(PTE2PA(*pte)) | perm | PTE_V;
    }
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
    uint64_t page_va = PGROUNDDOWN(dstva);
    pte_t *wpte = walk(pagetable, page_va, 0);
    if (!wpte)
      return -1;

    if (PTE_IS_SWAPPED(*wpte)) {
      int sflags = 0;
      int slot = pte_swap_take(wpte, &sflags);
      char *newpage = kalloc();
      if (!newpage || slot < 0) {
        if (newpage)
          kfree(newpage);
        return -1;
      }
      swap_read(slot, newpage);
      swap_free(slot);
      *wpte = PA2PTE((uint64_t)newpage) | sflags | PTE_V;
      asm volatile("sfence.vma %0, zero" : : "r"(page_va));
    }

    if ((*wpte & PTE_V) == 0 || (*wpte & PTE_U) == 0)
      return -1;

    if (*wpte & PTE_COW) {
      uint64_t pa = PTE2PA(*wpte);
      int flags = PTE_FLAGS(*wpte);

      if (page_ref_get(pa) == 1) {
        *wpte = PA2PTE(pa) | ((flags & ~PTE_COW) | PTE_W);
      } else {
        char *newpage = kalloc();
        if (!newpage)
          return -1;
        memcpy(newpage, (void *)pa, PAGE_SIZE);
        *wpte = PA2PTE((uint64_t)newpage) | ((flags & ~PTE_COW) | PTE_W);
        kfree((void *)pa);
      }
      asm volatile("sfence.vma %0, zero" : : "r"(page_va));
    }

    int level;
    pte_t *pte = walk_super(pagetable, dstva, &level);
    if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0 ||
        (*pte & PTE_W) == 0)
      return -1;

    uint64_t page_size = (level == 1) ? SUPERPAGE_SIZE : PAGE_SIZE;
    uint64_t va0 = (level == 1) ? SPGROUNDDOWN(dstva) : PGROUNDDOWN(dstva);
    uint64_t pa0 = PTE2PA(*pte) + (dstva - va0);
    uint64_t n = page_size - (dstva - va0);
    if (n > len)
      n = len;

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
