#include <kalloc.h>
#include <kmalloc.h>
#include <printk.h>
#include <riscv.h>
#include <string.h>
#include <vm.h>
#include <proc.h>
#include <mm.h>  /* BF: needed for anon_vma_get/link/unlink in vma_remove splits */
#include <errno.h>  /* BF: EFAULT for copyinstr page fault distinction */
#include <dtb.h>

#ifndef VM_FREEWALK_TRACE
#define VM_FREEWALK_TRACE 0
#endif

#ifndef PTE_DEBUG_FAILFAST
#define PTE_DEBUG_FAILFAST 0
#endif

#ifndef PTE_UPDATE_TRACE
#define PTE_UPDATE_TRACE 0
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

extern char _kernel_start[];
extern char _kernel_text_start[];
extern char _kernel_text_end[];
extern char _kernel_ro_start[];
extern char _kernel_ro_end[];
extern char _kernel_rw_start[];

static int pa_is_valid_page(uint64_t pa);
static int pa_is_valid_superpage(uint64_t pa);
static int pa_is_live_page(uint64_t pa);
static int pa_is_live_superpage(uint64_t pa);
static int pa_is_live_ptpage(uint64_t pa);
static int vm_check_pagetable_level(pagetable_t pt, int level,
                                    const char *label, int *printed);

static int kernel_page_expected_perm(uint64_t va, int *perm_out) {
  uint64_t page = PGROUNDDOWN(va);
  if (page < KERNBASE || page >= PHYSTOP)
    return 0;

  uint64_t text_start = PGROUNDDOWN((uint64_t)_kernel_text_start);
  uint64_t text_end = PGROUNDUP((uint64_t)_kernel_text_end);
  uint64_t ro_start = PGROUNDDOWN((uint64_t)_kernel_ro_start);
  uint64_t ro_end = PGROUNDUP((uint64_t)_kernel_ro_end);

  if (page >= text_start && page < text_end)
    *perm_out = PTE_R | PTE_X;
  else if (page >= ro_start && page < ro_end)
    *perm_out = PTE_R;
  else
    *perm_out = PTE_R | PTE_W;
  return 1;
}

int kernel_va_executable(uint64_t va) {
  int perm = 0;
  return kernel_page_expected_perm(va, &perm) && (perm & PTE_X);
}

enum pte_kind pte_classify(pte_t pte, int level) {
  if (!(pte & PTE_V))
    return PTE_KIND_EMPTY;
  if ((pte & PTE_W) && !(pte & PTE_R))
    return PTE_KIND_RESERVED;
  if (pte & (PTE_R | PTE_W | PTE_X)) {
    if (pte & PTE_COW)
      return PTE_KIND_COW_LEAF;
    return PTE_KIND_LEAF;
  }
  /* SBUnix encodes PROT_NONE as a valid L0 user PTE with no R/W/X bits.
   * Sv39 non-leaf entries also have no R/W/X bits, so never classify
   * upper-level entries as PROT_NONE. */
  if (level == 0 && (pte & PTE_U))
    return PTE_KIND_PROT_NONE;
  return PTE_KIND_NONLEAF;
}

const char *pte_kind_name(enum pte_kind kind) {
  switch (kind) {
  case PTE_KIND_EMPTY: return "empty";
  case PTE_KIND_NONLEAF: return "nonleaf";
  case PTE_KIND_LEAF: return "leaf";
  case PTE_KIND_COW_LEAF: return "cow-leaf";
  case PTE_KIND_PROT_NONE: return "prot-none";
  case PTE_KIND_RESERVED: return "reserved";
  default: return "unknown";
  }
}

const char *pte_update_reason_name(enum pte_update_reason reason) {
  switch (reason) {
  case PTE_UPDATE_PT_ALLOC: return "pt-alloc";
  case PTE_UPDATE_MAP: return "map";
  case PTE_UPDATE_SUPER_MAP: return "super-map";
  case PTE_UPDATE_RECURSIVE_MAP: return "recursive-map";
  case PTE_UPDATE_KERNEL_SHARE: return "kernel-share";
  case PTE_UPDATE_KERNEL_REPAIR: return "kernel-repair";
  case PTE_UPDATE_UNMAP: return "unmap";
  case PTE_UPDATE_COW: return "cow";
  case PTE_UPDATE_MPROTECT: return "mprotect";
  case PTE_UPDATE_MREMAP: return "mremap";
  case PTE_UPDATE_EXEC_PERM: return "exec-perm";
  case PTE_UPDATE_CLEAR_ACCESSED: return "clear-accessed";
  default: return "unknown";
  }
}

static int pte_update_fail(pagetable_t root, uint64_t va, pte_t oldpte,
                           pte_t newpte, int level,
                           enum pte_update_reason reason,
                           const char *why) {
  printk("pte-update violation: reason=%s root=0x%lx va=0x%lx level=%d "
         "old=0x%lx(%s) new=0x%lx(%s): %s\n",
         pte_update_reason_name(reason), (uint64_t)root, PGROUNDDOWN(va),
         level, oldpte, pte_kind_name(pte_classify(oldpte, level)),
         newpte, pte_kind_name(pte_classify(newpte, level)), why);
#if PTE_DEBUG_FAILFAST
  panic("pte_update invariant violation");
#endif
  return -1;
}

static int pte_leaf_pa_aligned(uint64_t pa, int level) {
  if (level == 0)
    return (pa % PAGE_SIZE) == 0;
  if (level == 1)
    return IS_SUPERPAGE_ALIGNED(pa);
  if (level == 2)
    return (pa & ((1UL << 30) - 1)) == 0;
  return 0;
}

static int pte_user_leaf_pa_ok(uint64_t pa, int level) {
  uint64_t count = (level == 1) ? SUPERPAGE_NPAGES : 1;
  for (uint64_t i = 0; i < count; i++) {
    uint64_t sub_pa = pa + i * PAGE_SIZE;
    enum page_frame_type type = page_get_type(sub_pa);
    if (type == PAGE_FREE || type == PAGE_PAGETABLE ||
        type == PAGE_SLAB || type == PAGE_DMA ||
        type == PAGE_KERNEL)
      return 0;
  }
  return 1;
}

static void pte_mark_user_leaf(uint64_t pa, int level) {
  uint64_t count = (level == 1) ? SUPERPAGE_NPAGES : 1;
  for (uint64_t i = 0; i < count; i++) {
    uint64_t sub_pa = pa + i * PAGE_SIZE;
    enum page_frame_type type = page_get_type(sub_pa);
    if (type == PAGE_UNKNOWN || type == PAGE_USER)
      page_set_type(sub_pa, PAGE_USER);
  }
}

static void pte_do_sfence(uint64_t va, enum pte_sfence_requirement sfence) {
  if (sfence == PTE_SFENCE_VA)
    asm volatile("sfence.vma %0, zero" : : "r"(PGROUNDDOWN(va)) : "memory");
  else if (sfence == PTE_SFENCE_ALL)
    asm volatile("sfence.vma zero, zero" ::: "memory");
}

int pte_update(pagetable_t root, uint64_t va, pte_t *slot, pte_t newpte,
               int level, enum pte_update_reason reason,
               enum pte_sfence_requirement sfence) {
  if (!slot)
    return -1;

  pte_t oldpte = *slot;
  enum pte_kind newkind = pte_classify(newpte, level);

  if (level < 0 || level > 2)
    return pte_update_fail(root, va, oldpte, newpte, level, reason,
                           "bad page-table level");

  if (newkind == PTE_KIND_RESERVED)
    return pte_update_fail(root, va, oldpte, newpte, level, reason,
                           "Sv39 reserved PTE encoding");

  if (newkind == PTE_KIND_NONLEAF) {
    uint64_t child_pa = PTE2PA(newpte);
    if (level == 0)
      return pte_update_fail(root, va, oldpte, newpte, level, reason,
                             "non-leaf PTE at level 0");
    if ((newpte & PTE_U) || (newpte & (PTE_R | PTE_W | PTE_X)))
      return pte_update_fail(root, va, oldpte, newpte, level, reason,
                             "non-leaf PTE has privilege or leaf bits");
    if (!pa_is_valid_page(child_pa) || !page_is_ptpage(child_pa))
      return pte_update_fail(root, va, oldpte, newpte, level, reason,
                             "non-leaf target is not a live page-table page");
  } else if (newkind == PTE_KIND_LEAF ||
             newkind == PTE_KIND_COW_LEAF ||
             newkind == PTE_KIND_PROT_NONE) {
    uint64_t pa = PTE2PA(newpte);
    if (!pte_leaf_pa_aligned(pa, level))
      return pte_update_fail(root, va, oldpte, newpte, level, reason,
                             "leaf physical address is misaligned for level");
    if ((newpte & PTE_W) && !(newpte & PTE_R))
      return pte_update_fail(root, va, oldpte, newpte, level, reason,
                             "leaf PTE grants W without R");
    if ((newpte & PTE_U) && (root == kernel_pagetable || !user_addr_ok(va)))
      return pte_update_fail(root, va, oldpte, newpte, level, reason,
                             "kernel-range PTE grants user access");
    if ((newpte & PTE_U)) {
      if (!pa_is_valid_page(pa) ||
          (level == 1 && !pa_is_valid_superpage(pa)) ||
          !pte_user_leaf_pa_ok(pa, level))
        return pte_update_fail(root, va, oldpte, newpte, level, reason,
                               "user leaf maps invalid or non-user-owned frame");
    } else {
      if (newpte & PTE_X) {
        int expected = 0;
        if (!kernel_page_expected_perm(va, &expected) || !(expected & PTE_X))
          return pte_update_fail(root, va, oldpte, newpte, level, reason,
                                 "kernel/MMIO leaf has unexpected execute");
      }
      if (root == kernel_pagetable && va >= KERNBASE && va < PHYSTOP) {
        int expected = 0;
        if (kernel_page_expected_perm(va, &expected)) {
          int actual = (int)(newpte & (PTE_R | PTE_W | PTE_X | PTE_U));
          if (actual != expected)
            return pte_update_fail(root, va, oldpte, newpte, level, reason,
                                   "kernel RAM leaf permission mismatch");
        }
      }
    }
  }

#if PTE_UPDATE_TRACE
  printk("pte-update: reason=%s root=0x%lx va=0x%lx level=%d old=0x%lx "
         "new=0x%lx sfence=%d\n",
         pte_update_reason_name(reason), (uint64_t)root, PGROUNDDOWN(va),
         level, oldpte, newpte, sfence);
#endif

  *slot = newpte;
  if (newpte & PTE_U) {
    enum pte_kind stored = pte_classify(newpte, level);
    if (stored == PTE_KIND_LEAF || stored == PTE_KIND_COW_LEAF ||
        stored == PTE_KIND_PROT_NONE)
      pte_mark_user_leaf(PTE2PA(newpte), level);
  }
  pte_do_sfence(va, sfence);
  return 0;
}

static int pte_walk_stops(pte_t pte, int level) {
  enum pte_kind kind = pte_classify(pte, level);
  return kind == PTE_KIND_LEAF ||
         kind == PTE_KIND_COW_LEAF ||
         kind == PTE_KIND_PROT_NONE ||
         kind == PTE_KIND_RESERVED;
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
// Detects leaf PTEs at level 1 (super pages) and returns them
// without descending further.
pte_t *walk(pagetable_t pagetable, uint64_t va, int alloc) {
    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pagetable[PX(level, va)];
        if (*pte & PTE_V) {
            if (pte_walk_stops(*pte, level)) {
                /* This is a super page (level 1) or giga page (level 2).
                 * Cannot descend further -- return this leaf PTE. */
                return pte;
            }
            if (pte_classify(*pte, level) != PTE_KIND_NONLEAF)
                return 0;
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
            if (pte_update(pagetable, va, pte, PA2PTE(pagetable) | PTE_V,
                           level, PTE_UPDATE_PT_ALLOC,
                           PTE_SFENCE_NONE) < 0) {
                kfree((void *)pagetable);
                return 0;
            }
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
            if (pte_walk_stops(*pte, level)) {
                if (level_out) *level_out = level;
                return pte;
            }
            if (pte_classify(*pte, level) != PTE_KIND_NONLEAF) {
                if (level_out) *level_out = -1;
                return 0;
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

/* BF: Internal flag for mappages_flags() — allow overwriting existing
 * valid PTEs.  Only used by kvmmap() which remaps MMIO/kernel regions
 * at boot.  All other callers map into known-invalid PTEs. */
#define MAP_FLAG_REMAP (1 << 0)

/* Create PTEs for virtual addresses starting at va that refer to
 * physical addresses starting at pa.  va and size might not
 * be page-aligned.
 *
 * flags: 0 for normal use, MAP_FLAG_REMAP to allow overwrite of
 *        existing valid PTEs. */
static int mappages_flags(pagetable_t pagetable, uint64_t va, uint64_t size,
                          uint64_t pa, int perm, int flags) {
    uint64_t a, last;
    pte_t *pte;

    if (size == 0) return 0;
    if (va + size < va) return -1; /* BF: Overflow guard — va + size wraps around */
    if (perm & PTE_W)
        perm |= PTE_R;

    a = PGROUNDDOWN(va);
    last = PGROUNDDOWN(va + size - 1);
    for(;;) {
        if((pte = walk(pagetable, a, 1)) == 0)
            return -1;
        /* BF: Detect accidental PTE overwrite — a valid PTE being silently
         * clobbered usually indicates a double-map bug.  Only kvmmap passes
         * MAP_FLAG_REMAP because boot-time MMIO regions legitimately remap. */
        if((*pte & PTE_V) && !(flags & MAP_FLAG_REMAP))
            return -1;
        if (pte_update(pagetable, a, pte, PA2PTE(pa) | perm | PTE_V,
                       0, PTE_UPDATE_MAP, PTE_SFENCE_NONE) < 0)
            return -1;
        if(a == last)
            break;
        a += PAGE_SIZE;
        pa += PAGE_SIZE;
    }
    return 0;
}

// Default wrapper: rejects overwrite of existing valid PTEs.
int mappages(pagetable_t pagetable, uint64_t va, uint64_t size, uint64_t pa, int perm) {
    return mappages_flags(pagetable, va, size, pa, perm, 0);
}

// add a mapping to the kernel page table.
// only used when booting.
// BF: Uses MAP_FLAG_REMAP because boot remaps MMIO/kernel regions
// that may already have valid PTEs from earlier init steps.
void kvmmap(uint64_t va, uint64_t pa, uint64_t sz, int perm) {
    if(mappages_flags(kernel_pagetable, va, sz, pa, perm, MAP_FLAG_REMAP) != 0)
        printk("kvmmap failed\n");
}

// Create level-1 leaf PTEs (2MB super page mappings).
// va, pa must be 2MB-aligned; size must be a multiple of 2MB.
int mappages_super(pagetable_t pagetable, uint64_t va, uint64_t size,
                   uint64_t pa, int perm) {
    if (!IS_SUPERPAGE_ALIGNED(va) || !IS_SUPERPAGE_ALIGNED(pa) ||
        !IS_SUPERPAGE_ALIGNED(size))
        return -1;
    if (perm & PTE_W)
        perm |= PTE_R;

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
            if (pte_update(pagetable, va, l2pte, PA2PTE(l1pt) | PTE_V,
                           2, PTE_UPDATE_PT_ALLOC,
                           PTE_SFENCE_NONE) < 0) {
                kfree((void *)l1pt);
                return -1;
            }
        }

        pte_t *l1pte = &l1pt[PX(1, va)];
        /* Install leaf PTE at level 1 */
        if (pte_update(pagetable, va, l1pte, PA2PTE(pa) | perm | PTE_V,
                       1, PTE_UPDATE_SUPER_MAP,
                       PTE_SFENCE_NONE) < 0)
            return -1;
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

    struct dtb_device *rtc = dtb_find_device(DTB_DEV_RTC);
    if (rtc) {
      kvmmap(rtc->base_addr, rtc->base_addr,
             rtc->size ? rtc->size : PAGE_SIZE, PTE_R | PTE_W);
    } else {
      kvmmap(0x101000, 0x101000, PAGE_SIZE, PTE_R | PTE_W);
    }

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

    /* Kernel RAM is mapped with W^X permissions.  The executable text,
     * read-only data, and writable kernel heap/data ranges intentionally
     * use 4KB leaves so no kernel RAM page is both writable and executable.
     * The small RAM prefix below the linked kernel image is kept RW/NX for
     * compatibility with the virt memory layout. */
    uint64_t image_start = PGROUNDDOWN((uint64_t)_kernel_start);
    uint64_t text_start = PGROUNDDOWN((uint64_t)_kernel_text_start);
    uint64_t text_end = PGROUNDUP((uint64_t)_kernel_text_end);
    uint64_t ro_start = PGROUNDDOWN((uint64_t)_kernel_ro_start);
    uint64_t ro_end = PGROUNDUP((uint64_t)_kernel_ro_end);
    uint64_t rw_start = PGROUNDDOWN((uint64_t)_kernel_rw_start);

    if (image_start > KERNBASE)
      kvmmap(KERNBASE, KERNBASE, image_start - KERNBASE, PTE_R | PTE_W);
    if (text_end > text_start)
      kvmmap(text_start, text_start, text_end - text_start, PTE_R | PTE_X);
    if (ro_end > ro_start)
      kvmmap(ro_start, ro_start, ro_end - ro_start, PTE_R);
    if (PHYSTOP > rw_start)
      kvmmap(rw_start, rw_start, PHYSTOP - rw_start, PTE_R | PTE_W);

    /* Sv39 self-referencing page table: install a 1GB leaf gigapage
     * at L2[510] that maps physical RAM [0x80000000, 0xC0000000) to
     * a dedicated read-only VA window for diagnostics.
     *
     * Leaf gigapage: PTE_R makes this a leaf at L2 level.
     * No PTE_W, because a writable physical-memory alias would bypass
     * the kernel text/rodata write protection above.
     * No PTE_X (page table data should not be executable).
     * No PTE_U (kernel-only access). */
    pte_update(kernel_pagetable, RECURSIVE_BASE,
               &kernel_pagetable[RECURSIVE_INDEX],
               PA2PTE(0x80000000UL) | PTE_V | PTE_R,
               2, PTE_UPDATE_RECURSIVE_MAP, PTE_SFENCE_NONE);

    // Install the kernel page table (write to satp)
    w_satp(MAKE_SATP(kernel_pagetable));
    sfence_vma(); // flush TLB

    printk("kvminit: paging enabled (kernel W^X).\n");
#if PTE_DEBUG_FAILFAST
    printk("kvminit: PTE_DEBUG_FAILFAST=1 active.\n");
#endif
}

/* Kernel mapping repair/audit helpers.  Historical PTE_X corruption repair
 * used to blindly set execute on kernel RAM superpages.  With W^X mappings,
 * repair must enforce the expected permission for each page instead. */
static int kernel_pte_repair_count;

static int enforce_kernel_pte(uint64_t va, int noisy) {
  int expected = 0;
  int level = -1;
  pte_t *pte;

  if (!kernel_page_expected_perm(va, &expected))
    return 0;

  pte = walk_super(kernel_pagetable, PGROUNDDOWN(va), &level);
  if (!pte || !(*pte & PTE_V) || !(*pte & (PTE_R | PTE_W | PTE_X)))
    return 0;

  pte_t old = *pte;
  pte_t newpte = (old & ~(PTE_R | PTE_W | PTE_X | PTE_U)) | expected | PTE_V;
  if (newpte == old)
    return 0;

#if PTE_DEBUG_FAILFAST
  printk("kernel PTE repair requested: va=0x%lx pte=0x%lx -> 0x%lx sepc=0x%lx\n",
         PGROUNDDOWN(va), (unsigned long)old, (unsigned long)newpte,
         (unsigned long)r_sepc());
  panic("kernel PTE repair requested");
#endif

  kernel_pte_repair_count++;
  if (noisy) {
    printk("kernel PTE repair #%d: va=0x%lx pte=0x%lx -> 0x%lx sepc=0x%lx\n",
           kernel_pte_repair_count, PGROUNDDOWN(va),
           (unsigned long)old, (unsigned long)newpte,
           (unsigned long)r_sepc());
  }
  if (pte_update(kernel_pagetable, PGROUNDDOWN(va), pte, newpte, level,
                 PTE_UPDATE_KERNEL_REPAIR, PTE_SFENCE_NONE) < 0)
    return 0;
  return 1;
}

int recursive_repair_kernel_pte(uint64_t va) {
  int repaired = enforce_kernel_pte(va, 1);
  if (repaired) {
    asm volatile("sfence.vma zero, zero" ::: "memory");
    if (kernel_va_executable(va))
      asm volatile("fence.i" ::: "memory");
  }
  return repaired;
}

int recursive_check_kcode_pte(void) {
  return recursive_repair_kernel_pte((uint64_t)_kernel_text_start);
}

int recursive_check_all_kernel_ptes(void) {
  int repairs = 0;
  for (uint64_t va = KERNBASE; va < PHYSTOP; va += PAGE_SIZE)
    repairs += enforce_kernel_pte(va, 0);
  if (repairs > 0) {
    asm volatile("sfence.vma zero, zero" ::: "memory");
    asm volatile("fence.i" ::: "memory");
  }
  return repairs;
}

int kernel_wx_audit(void) {
  int bad = 0;
  for (uint64_t va = KERNBASE; va < PHYSTOP; va += PAGE_SIZE) {
    int expected = 0;
    int level = -1;
    pte_t *pte;

    if (!kernel_page_expected_perm(va, &expected))
      continue;
    pte = walk_super(kernel_pagetable, va, &level);
    if (!pte || !(*pte & PTE_V) || !(*pte & (PTE_R | PTE_W | PTE_X))) {
      bad++;
      continue;
    }

    pte_t flags = *pte & (PTE_R | PTE_W | PTE_X | PTE_U);
    if ((flags & (PTE_W | PTE_X)) == (PTE_W | PTE_X))
      bad++;
    if (flags != (uint64_t)expected)
      bad++;
  }
  return bad;
}

/* Map kernel page table entries into a user page table by sharing
 * references (not copying page table pages).  This avoids the ~65
 * page table page leak that occurred when kernel entries were
 * duplicated per-process. */
int uvmmap_kernel(pagetable_t pt) {
  /* Share VPN[2]=1..511: these root entries cover kernel RAM, PCIe MMIO BAR,
   * and other regions entirely above user space. */
  for (int i = 1; i < 512; i++) {
    uint64_t va = (uint64_t)i << 30;
    if (pte_update(pt, va, &pt[i], kernel_pagetable[i],
                   2, PTE_UPDATE_KERNEL_SHARE,
                   PTE_SFENCE_NONE) < 0)
      return -1;
  }

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
    if ((kernel_L1[j] & PTE_V) && !(user_L1[j] & PTE_V)) {
      uint64_t va = (uint64_t)j * SUPERPAGE_SIZE;
      if (pte_update(pt, va, &user_L1[j], kernel_L1[j],
                     1, PTE_UPDATE_KERNEL_SHARE,
                     PTE_SFENCE_NONE) < 0)
        return -1;
    }
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
//
// When the owning proc is provided, 4KB anonymous pages use reverse-map
// teardown first so the page-descriptor path owns the final free.  Callers
// without a live proc/VMA context can pass NULL and fall back to raw kfree.
void uvmunmap_proc(struct proc *p, pagetable_t pagetable, uint64_t va, uint64_t len) {
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
                int freed_via_rmap = 0;
                if (p) {
                    struct vm_area *v = vma_find(p, a);
                    if (v) {
                        struct page *pg = virt_to_page((void *)pa);
                        if (pg) {
                            page_remove_rmap(pg, v);
                            freed_via_rmap = 1;
                        }
                    }
                }
                if (!freed_via_rmap)
                    kfree((void *)pa);
                *pte = 0;
                a += PAGE_SIZE;
            }
        } else {
            /* Completely unmapped */
            a += PAGE_SIZE;
        }
    }
}

void uvmunmap(pagetable_t pagetable, uint64_t va, uint64_t len) {
    uvmunmap_proc(NULL, pagetable, va, len);
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
    if (pte_update(pagetable, va, pte, 0, level,
                   PTE_UPDATE_UNMAP, PTE_SFENCE_NONE) < 0)
        return;

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

static int vma_can_merge(struct vm_area *a, struct vm_area *b) {
  if (!a || !b)
    return 0;
  if (a->end < b->start || a->prot != b->prot || a->flags != b->flags)
    return 0;
  if (a->file || b->file)
    return 0;
  /* BF: Anonymous VMAs carry anon_vma chains, page offsets, and resident
   * counts.  Do not merge them until that metadata can be combined
   * without losing reverse-map ownership. */
  if ((a->flags & MAP_ANON) || (b->flags & MAP_ANON))
    return 0;
  if (a->anon_vma || b->anon_vma ||
      !list_empty(&a->anon_vma_chain) || !list_empty(&b->anon_vma_chain))
    return 0;
  if (a->resident_pages || b->resident_pages)
    return 0;
  return 1;
}

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
  while (vma->next && vma_can_merge(vma, vma->next)) {
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
    if (prev->next == vma && vma_can_merge(prev, vma)) {
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
      /* BF: anon_vma_link() calls anon_vma_get() for the chain entry.
       * We need a SEPARATE anon_vma_get() for the VMA's own anon_vma
       * pointer, because anon_vma_unlink does one put per chain plus
       * one put for vma->anon_vma (2 puts total for 1 chain). */
      if (v->anon_vma) {
        tail->anon_vma = v->anon_vma;
        anon_vma_get(v->anon_vma);
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
        vfs_close_file(v->file);
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
      vfs_close_file(v->file);
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

static int vm_check_pagetable_level(pagetable_t pt, int level,
                                    const char *label, int *printed) {
  int errors = 0;
  for (int i = 0; i < 512; i++) {
    pte_t pte = pt[i];
    enum pte_kind kind = pte_classify(pte, level);
    if (kind == PTE_KIND_EMPTY)
      continue;

    if (kind == PTE_KIND_RESERVED) {
      errors++;
      if (*printed < 32) {
        printk("pte-check[%s]: reserved pte lvl=%d idx=%d pte=0x%lx\n",
               label ? label : "?", level, i, pte);
        (*printed)++;
      }
      continue;
    }

    if ((kind == PTE_KIND_LEAF || kind == PTE_KIND_COW_LEAF ||
         kind == PTE_KIND_PROT_NONE) && level == 1 &&
        !IS_SUPERPAGE_ALIGNED(PTE2PA(pte))) {
      errors++;
      if (*printed < 32) {
        printk("pte-check[%s]: unaligned superpage lvl=%d idx=%d pte=0x%lx pa=0x%lx\n",
               label ? label : "?", level, i, pte, PTE2PA(pte));
        (*printed)++;
      }
    }

    if (kind == PTE_KIND_NONLEAF) {
      uint64_t child_pa = PTE2PA(pte);
      if (level == 0 || !pa_is_valid_page(child_pa)) {
        errors++;
        if (*printed < 32) {
          printk("pte-check[%s]: bad nonleaf lvl=%d idx=%d pte=0x%lx pa=0x%lx\n",
                 label ? label : "?", level, i, pte, child_pa);
          (*printed)++;
        }
        continue;
      }
      errors += vm_check_pagetable_level((pagetable_t)child_pa, level - 1,
                                         label, printed);
    }
  }
  return errors;
}

int vm_check_pagetable(pagetable_t pagetable, const char *label) {
  if (!pagetable)
    return 1;
  int printed = 0;
  int errors = vm_check_pagetable_level(pagetable, 2, label, &printed);
  if (errors)
    printk("pte-check[%s]: %d invariant violation(s)\n",
           label ? label : "?", errors);
  return errors;
}

/* Recursively free page-table pages and user leaf pages in a sub-tree.
 * Kernel-only leaf entries (no PTE_U) are skipped so MMIO and kernel
 * RAM are never kfree'd.  Page-table pages themselves are always freed.
 * Leaf pages are freed via kfree (which decrements refcount for COW). */
static void freewalk_level(pagetable_t pt, int level) {
  for (int i = 0; i < 512; i++) {
    pte_t pte = pt[i];
    enum pte_kind kind = pte_classify(pte, level);
    if (kind == PTE_KIND_EMPTY)
      continue;
    if (kind == PTE_KIND_LEAF ||
               kind == PTE_KIND_COW_LEAF ||
               kind == PTE_KIND_PROT_NONE) {
      if (!(pte & PTE_U)) {
        /* Kernel-only leaf entries point to MMIO/kernel RAM. */
        continue;
      }
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
    } else if (kind == PTE_KIND_NONLEAF) {
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
    } else if (kind == PTE_KIND_RESERVED) {
      pt[i] = 0;
    }
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
      }
    }

    int printed = 0;
    int errors = vm_check_pagetable_level(user_L1, 1,
                                          "proc_freepagetable-user",
                                          &printed);
    if (errors) {
      printk("pte-check[proc_freepagetable-user]: %d invariant violation(s)\n",
             errors);
    }

    for (int j = 0; j < 512; j++) {
      pte_t pte = user_L1[j];
      enum pte_kind kind = pte_classify(pte, 1);
      if (kind == PTE_KIND_EMPTY) {
        continue;
      } else if (kind == PTE_KIND_LEAF ||
                 kind == PTE_KIND_COW_LEAF ||
                 kind == PTE_KIND_PROT_NONE) {
        /* User-owned entry — free recursively. A user leaf may have no R/W/X
         * bits after mprotect(PROT_NONE), so classify before non-leaf. */
        if (pte & PTE_U) {
          uint64_t pa = PTE2PA(pte);
          if (pa_is_live_superpage(pa)) {
            vm_trace_freewalk("l1-super", user_L1, 1, j, pte, pa);
            kfree_super((void *)pa);
          }
          user_L1[j] = 0;
        }
      } else if (kind == PTE_KIND_NONLEAF) {
          /* Non-leaf: recurse into level-0 table */
          pagetable_t child = (pagetable_t)PTE2PA(pte);
#if VM_FREEWALK_TRACE
          pte_t saved_pte = pte;  /* save for trace after clearing */
#endif
          if (pa_is_live_ptpage((uint64_t)child)) {
            /* Clear PTE before recursion to prevent double-free */
            user_L1[j] = 0;
            freewalk_level(child, 0);
#if VM_FREEWALK_TRACE
            vm_trace_freewalk("l1-child", user_L1, 1, j, saved_pte, (uint64_t)child);
#endif
            kfree((void *)child);
          } else {
            /* Corrupted/reused L0 page — silently detach */
            user_L1[j] = 0;
          }
      } else if (kind == PTE_KIND_RESERVED) {
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

/* BF: Centralized COW break helper — handles ref count, page copy,
 * PTE update, and rmap registration for new private page.  Previously
 * the COW path in trap.c and copyout() duplicated this logic without
 * calling page_add_anon_rmap, leaving new private pages invisible
 * to the reverse-map eviction scanner. */
int cow_break(pagetable_t pagetable, pte_t *pte, uint64_t va,
              struct vm_area *vma) {
  uint64_t pa = PTE2PA(*pte);
  int flags = PTE_FLAGS(*pte);
  int ref = page_ref_get(pa);

  if (ref <= 1) {
    /* Sole reference: flip PTE flags in place.  Skip rmap registration.
     * uvmunmap does NOT call page_remove_rmap, so the page's original
     * rmap entry (if any) survives parent exit.  Adding rmap again
     * here would double-count. */
    if (pte_update(pagetable, va, pte,
                   PA2PTE(pa) | ((flags & ~PTE_COW) | PTE_W),
                   0, PTE_UPDATE_COW, PTE_SFENCE_NONE) < 0)
      return -1;
  } else {
    char *newpage = kalloc();
    if (!newpage) return -1;
    memcpy(newpage, (void *)pa, PAGE_SIZE);
    if (pte_update(pagetable, va, pte,
                   PA2PTE((uint64_t)newpage) |
                   ((flags & ~PTE_COW) | PTE_W),
                   0, PTE_UPDATE_COW, PTE_SFENCE_NONE) < 0) {
      kfree(newpage);
      return -1;
    }
    /* Register the replacement before dropping the old page reference. */
    if (vma && vma->anon_vma) {
      struct page *pg = virt_to_page(newpage);
      if (pg) page_add_anon_rmap(pg, vma, va);
    }
    page_ref_dec(pa);
  }
  asm volatile("sfence.vma %0, zero" : : "r"(va));
  return 0;
}

/* BF: Resolve a user page that may be lazily unmapped or COW-shared.
 * Called from copyin/copyout/copyinstr when the initial
 * walk finds no valid PTE.
 *
 * Returns the physical address of the resolved page, or 0 on failure.
 * For write==1, also handles COW break.
 *
 * Only demand-allocates anonymous VMAs to avoid deadlock (file-backed
 * demand paging would re-enter VFS read paths). */
static uint64_t vm_resolve_user_page(struct proc *p, pagetable_t pagetable,
                                     uint64_t va, int write) {
  if (!p || pagetable != p->pagetable)
    return 0;

  /* Recursion guard: if we're already inside a demand-fault from a copy
   * path, bail out to prevent infinite recursion on the 8KB kernel stack. */
  if (p->in_copy_fault)
    return 0;
  p->in_copy_fault = 1;

  uint64_t result = 0;
  uint64_t page_va = PGROUNDDOWN(va);

  /* Look up VMA to validate the access */
  struct vm_area *vma = vma_find(p, page_va);
  if (!vma) {
    /* No VMA covers this address — EFAULT */
    goto out;
  }

  /* Validate permissions against VMA prot bits */
  if (vma->prot == 0) /* PROT_NONE — no access allowed */
    goto out;
  if (write && !(vma->prot & PROT_WRITE))
    goto out;
  if (!write && !(vma->prot & PROT_READ))
    goto out;

  pte_t *pte = walk(pagetable, page_va, 0);

  pte = walk(pagetable, page_va, 0);

  if (!pte || !(*pte & PTE_V)) {
    /* Page not present — demand-allocate for anonymous VMAs only.
     * File-backed VMAs are skipped to avoid VFS re-entry deadlock. */
    if (vma->file)
      goto out;
    char *pg = kalloc();
    if (!pg) goto out;
    memset(pg, 0, PAGE_SIZE);
    int perm = PTE_U | prot_to_pte_flags(vma->prot);
    if (mappages(pagetable, page_va, PAGE_SIZE, (uint64_t)pg, perm) != 0) {
      kfree(pg);
      goto out;
    }
    asm volatile("sfence.vma %0, zero" : : "r"(page_va));
    /* Register in rmap */
    if (vma->anon_vma) {
      struct page *ppg = virt_to_page(pg);
      if (ppg) page_add_anon_rmap(ppg, vma, page_va);
    }
    /* Re-walk to get the PTE we just installed */
    pte = walk(pagetable, page_va, 0);
    if (!pte || !(*pte & PTE_V))
      goto out;
  }

  /* Handle COW for write access */
  if (write && (*pte & PTE_COW)) {
    if (cow_break(pagetable, pte, page_va, vma) < 0)
      goto out;
  }

  /* Return the resolved physical address for the requested VA */
  result = PTE2PA(*pte) + (va - page_va);

out:
  p->in_copy_fault = 0;
  return result;
}

static int pte_is_user_leaf(pte_t pte) {
  enum pte_kind kind = pte_classify(pte, 0);
  return (pte & PTE_V) && (pte & PTE_U) &&
         (kind == PTE_KIND_LEAF || kind == PTE_KIND_COW_LEAF);
}

static int vma_allows_write(struct proc *p, uint64_t va) {
  if (!p)
    return 0;
  struct vm_area *vma = vma_find(p, PGROUNDDOWN(va));
  return vma && (vma->prot & PROT_WRITE);
}

static int copyout_impl(struct proc *p, pagetable_t pagetable, uint64_t dstva,
                        const void *src, uint64_t len) {
  const char *s = (const char *)src;
  while (len > 0) {
    if (!user_addr_ok(dstva))
      return -1;
    int level;
    pte_t *pte = walk_super(pagetable, dstva, &level);

    /* BF: If the page is not mapped, try demand-fault resolution.
     * This handles lazy brk pages that have not been faulted in yet
     * (e.g., write() to freshly brk'd memory). */
    if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0) {
      uint64_t pa = vm_resolve_user_page(p, pagetable, dstva, 1);
      if (pa == 0)
        return -1;
      /* After resolution, re-walk to get the proper PTE and level */
      pte = walk_super(pagetable, dstva, &level);
      if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
        return -1;
    }
    if (!pte_is_user_leaf(*pte))
      return -1;

    /* BF: Resolve COW page before writing via cow_break() which also
     * registers the new private page in anon rmap.  Also handle the case
     * where PTE_COW was lost but the page is still shared (ref > 1):
     * must copy before writing. */
    if (level == 0) {
      uint64_t old_pa = PTE2PA(*pte);
      int ref = page_ref_get(old_pa);
      if ((*pte & PTE_W) == 0 && !vma_allows_write(p, dstva))
        return -1;
      if ((*pte & PTE_COW) || (ref > 1 && !(*pte & PTE_W))) {
        /* Use cow_break for proper rmap registration of new page */
        uint64_t va_page = PGROUNDDOWN(dstva);
        struct vm_area *vma = p ? vma_find(p, va_page) : 0;
        if (cow_break(pagetable, pte, va_page, vma) < 0)
          return -1;
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

static int copyin_impl(struct proc *p, pagetable_t pagetable, void *dst,
                       uint64_t srcva, uint64_t len) {
  char *d = (char *)dst;
  while (len > 0) {
    if (!user_addr_ok(srcva))
      return -1;
    int level;
    pte_t *pte = walk_super(pagetable, srcva, &level);

    /* BF: If the page is not mapped, try demand-fault resolution.
     * This handles lazy brk pages and swapped-out pages that haven't
     * been faulted in yet (e.g., read() from freshly brk'd memory). */
    if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0) {
      uint64_t pa = vm_resolve_user_page(p, pagetable, srcva, 0);
      if (pa == 0)
        return -1;
      pte = walk_super(pagetable, srcva, &level);
      if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
        return -1;
    }
    if (!pte_is_user_leaf(*pte) || (*pte & PTE_R) == 0)
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

static int copyinstr_impl(struct proc *p, pagetable_t pagetable, char *dst,
                          uint64_t srcva, uint64_t max) {
  int got_null = 0;
  while (max > 0) {
    /* BF: Return -EFAULT for invalid user addresses so callers can
     * distinguish page faults from buffer exhaustion (-1 / E2BIG). */
    if (!user_addr_ok(srcva))
      return -EFAULT;
    int level;
    pte_t *pte = walk_super(pagetable, srcva, &level);

    /* BF: If the page is not mapped, try demand-fault resolution.
     * This handles lazy pages during string copy (e.g., execve argv).
     * Return -EFAULT (not -1) so callers can distinguish page faults
     * from buffer exhaustion (E2BIG). */
    if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0) {
      uint64_t pa = vm_resolve_user_page(p, pagetable, srcva, 0);
      if (pa == 0)
        return -EFAULT;
      pte = walk_super(pagetable, srcva, &level);
      if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
        return -EFAULT;
    }
    if (!pte_is_user_leaf(*pte) || (*pte & PTE_R) == 0)
      return -EFAULT;

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
  /* Buffer exhaustion: string did not fit in max bytes */
  return got_null ? 0 : -1;
}

// Copy from kernel to user for an explicitly-owned process pagetable.
int copyout_proc(struct proc *p, uint64_t dstva, const void *src, uint64_t len) {
  if (!p)
    return -1;
  return copyout_impl(p, p->pagetable, dstva, src, len);
}

int copyin_proc(struct proc *p, void *dst, uint64_t srcva, uint64_t len) {
  if (!p)
    return -1;
  return copyin_impl(p, p->pagetable, dst, srcva, len);
}

int copyinstr_proc(struct proc *p, char *dst, uint64_t srcva, uint64_t max) {
  if (!p)
    return -EFAULT;
  return copyinstr_impl(p, p->pagetable, dst, srcva, max);
}

// Legacy wrappers may only resolve lazy-allocation and COW faults for current_proc.
int copyout(pagetable_t pagetable, uint64_t dstva, const void *src, uint64_t len) {
  struct proc *owner =
      (current_proc && pagetable == current_proc->pagetable) ? current_proc : NULL;
  return copyout_impl(owner, pagetable, dstva, src, len);
}

int copyin(pagetable_t pagetable, void *dst, uint64_t srcva, uint64_t len) {
  struct proc *owner =
      (current_proc && pagetable == current_proc->pagetable) ? current_proc : NULL;
  return copyin_impl(owner, pagetable, dst, srcva, len);
}

int copyinstr(pagetable_t pagetable, char *dst, uint64_t srcva, uint64_t max) {
  struct proc *owner =
      (current_proc && pagetable == current_proc->pagetable) ? current_proc : NULL;
  return copyinstr_impl(owner, pagetable, dst, srcva, max);
}

/* ENH: Insert VMA into an arbitrary list head (for staged exec).
 * This allows building a VMA list independently of process state. */
void vma_add_to_list(struct vm_area **list_head, struct vm_area *vma) {
  vma->next = *list_head;
  *list_head = vma;
}

/* ENH: Free an entire VMA chain without touching process state.
 * Used to discard staged VMAs on exec failure without corrupting
 * the original process's VMA list.
 * BF: Must release file references and unlink anon_vma chains,
 * matching vma_free_all's cleanup — otherwise file refcounts leak
 * and stale anon_vma pointers cause use-after-free during reclaim. */
void vma_free_chain(struct vm_area *head) {
  while (head) {
    struct vm_area *next = head->next;
    if (head->file) {
      vfs_close_file(head->file);
      head->file = NULL;
    }
    if ((head->flags & MAP_ANON) && head->anon_vma) {
      anon_vma_unlink(head);
    }
    kmfree(head, sizeof(struct vm_area));
    head = next;
  }
}
