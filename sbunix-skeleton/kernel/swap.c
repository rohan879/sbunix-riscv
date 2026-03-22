#include <swap.h>
#include <kalloc.h>
#include <printk.h>
#include <riscv.h>
#include <string.h>
#include <proc.h>
#include <vm.h>
#include <virtio_blk.h>

/* ================================================================== */
/* Swap bitmap                                                         */
/* ================================================================== */
#define SWAP_BITMAP_WORDS  ((SWAP_NSLOTS + 63) / 64)
static uint64_t swap_bitmap[SWAP_BITMAP_WORDS];
static int swap_used_count;
static uint64_t swap_start_sector;
static int swap_is_enabled;

extern char _kernel_end[];

/* Clock algorithm state (persistent across calls) */
static int clock_proc_idx;
static uint64_t clock_va;

/* ================================================================== */
/* Initialization                                                      */
/* ================================================================== */
void swap_init(void) {
  swap_is_enabled = 0;
  swap_used_count = 0;
  clock_proc_idx = 0;
  clock_va = 0;
  memset(swap_bitmap, 0, sizeof(swap_bitmap));

  uint64_t cap = virtio_blk_capacity();
  if (cap == 0) {
    printk("swap: no disk available, swap disabled\n");
    return;
  }

  if (cap < SWAP_NSECTORS + 128) {
    /* Disk too small: need at least swap region + some fs space */
    printk("swap: disk too small (%ld sectors), swap disabled\n", (long)cap);
    return;
  }

  swap_start_sector = cap - SWAP_NSECTORS;
  swap_is_enabled = 1;

  printk("swap: initialized %d slots (%d KB) at sector %ld\n",
         SWAP_NSLOTS, SWAP_NSLOTS * 4, (long)swap_start_sector);
}

int swap_enabled(void) {
  return swap_is_enabled;
}

/* ================================================================== */
/* Bitmap operations                                                   */
/* ================================================================== */

/* Allocate a free swap slot.  Returns slot index [0, SWAP_NSLOTS),
 * or -1 if swap is full. */
int swap_alloc(void) {
  if (!swap_is_enabled)
    return -1;

  for (int w = 0; w < SWAP_BITMAP_WORDS; w++) {
    if (swap_bitmap[w] == ~0UL)
      continue; /* all bits set, skip */
    /* Find first zero bit with linear scan */
    uint64_t word = swap_bitmap[w];
    for (int bit = 0; bit < 64; bit++) {
      if (!(word & (1UL << bit))) {
        int slot = w * 64 + bit;
        if (slot >= SWAP_NSLOTS)
          return -1;
        swap_bitmap[w] |= (1UL << bit);
        swap_used_count++;
        return slot;
      }
    }
  }
  return -1;
}

/* Free a swap slot, marking it available for reuse.
 * Idempotent: silently ignores already-free slots to tolerate
 * process-exit paths that may race with swap-in. */
void swap_free(int slot) {
  if (slot < 0 || slot >= SWAP_NSLOTS)
    return;
  int w = slot / 64;
  int bit = slot % 64;
  if (!(swap_bitmap[w] & (1UL << bit)))
    return; /* already free — nothing to do */
  swap_bitmap[w] &= ~(1UL << bit);
  swap_used_count--;
}

/* ================================================================== */
/* Disk I/O                                                            */
/* ================================================================== */

/* Write a 4 KB page to swap slot. */
void swap_write(int slot, void *page) {
  uint64_t sector = swap_start_sector + (uint64_t)slot * SECTORS_PER_PAGE;
  virtio_blk_rw(sector, page, 1);
}

/* Read a 4 KB page from swap slot. */
void swap_read(int slot, void *page) {
  uint64_t sector = swap_start_sector + (uint64_t)slot * SECTORS_PER_PAGE;
  virtio_blk_rw(sector, page, 0);
}

/* ================================================================== */
/* Eviction: clock (second-chance) algorithm                           */
/* ================================================================== */

void swap_evict_pages(int n) {
  if (!swap_is_enabled)
    return;

  int evicted = 0;
  /* Limit scan iterations to avoid infinite loops.
   * We scan at most 2 full passes over all 64 process slots with
   * a reasonable per-process VA step limit. */
  int max_iterations = 64 * 2 * 256;
  int iterations = 0;

  while (evicted < n && iterations < max_iterations) {
    iterations++;

    struct proc *p = &proc[clock_proc_idx];

    /* Skip unused processes and processes without user page tables */
    if (p->state == UNUSED || p->pagetable == 0 ||
        p->pagetable == kernel_pagetable) {
      clock_proc_idx = (clock_proc_idx + 1) % 64;
      clock_va = 0;
      continue;
    }

    /* Skip the currently running process (optimization) */
    if (p == current_proc) {
      clock_proc_idx = (clock_proc_idx + 1) % 64;
      clock_va = 0;
      continue;
    }

    pte_t *pte = walk(p->pagetable, clock_va, 0);

    if (!pte || !(*pte & PTE_V))
      goto advance;

    /* Only evict leaf PTEs (those with R, W, or X set) */
    if ((*pte & (PTE_R | PTE_W | PTE_X)) == 0)
      goto advance;

    /* Only evict user pages */
    if (!(*pte & PTE_U))
      goto advance;

    {
      uint64_t pa = PTE2PA(*pte);

      /* Skip kernel pages (below _kernel_end) */
      if (pa < (uint64_t)_kernel_end)
        goto advance;

      /* Skip COW-shared pages (refcount > 1) */
      if (page_ref_get(pa) > 1)
        goto advance;

      /* Second chance: check accessed bit (bit 6 in RISC-V Sv39) */
      if (*pte & (1L << 6)) {
        /* Clear accessed bit, give second chance */
        *pte &= ~(1L << 6);
        asm volatile("sfence.vma %0, zero" : : "r"(clock_va));
        goto advance;
      }

      /* This page is the victim -- evict it */
      int slot = swap_alloc();
      if (slot < 0)
        break; /* swap full, stop evicting */

      swap_write(slot, (void *)pa);
      int orig_flags = PTE_FLAGS(*pte);
      *pte = PTE_SWAP_ENCODE(slot, orig_flags);
      asm volatile("sfence.vma %0, zero" : : "r"(clock_va));
      kfree((void *)pa);
      evicted++;
    }

advance:
    clock_va += PAGE_SIZE;
    if (clock_va >= MAXVA) {
      clock_proc_idx = (clock_proc_idx + 1) % 64;
      clock_va = 0;
    }
  }
}

/* ================================================================== */
/* Debug / stats                                                       */
/* ================================================================== */
void swap_stats(void) {
  printk("swap: %d/%d slots used, enabled=%d\n",
         swap_used_count, SWAP_NSLOTS, swap_is_enabled);
}
