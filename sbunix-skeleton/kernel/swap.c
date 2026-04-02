#include <swap.h>
#include <kalloc.h>
#include <printk.h>
#include <riscv.h>
#include <string.h>
#include <proc.h>
#include <vm.h>
#include <virtio_blk.h>
#include "include/mm.h"

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
/* Eviction: enhanced clock algorithm with page descriptor integration */
/* ================================================================== */

int swap_evict_pages(int n) {
  if (!swap_is_enabled)
    return 0;

  int evicted = 0;
  /* Limit scan iterations to avoid infinite loops.
   * We scan at most 2 full passes over all 64 process slots with
   * a reasonable per-process VA step limit. */
  int max_iterations = 64 * 2 * 256;
  int iterations = 0;
  
  /* Enhanced clock algorithm statistics */
  extern struct mm_stat mm_stats;
  mm_stats.clock_scans++;

  while (evicted < n && iterations < max_iterations) {
    iterations++;

    struct proc *p = &proc[clock_proc_idx];

    /* Skip unused processes and processes without user page tables */
    if (p->state == UNUSED || p->pagetable == 0 ||
        p->pagetable == kernel_pagetable) {
      clock_proc_idx = (clock_proc_idx + 1) % NPROC;
      clock_va = 0;
      continue;
    }

    /* Skip the currently running process (optimization) */
    if (p == current_proc) {
      clock_proc_idx = (clock_proc_idx + 1) % NPROC;
      clock_va = 0;
      continue;
    }

    pte_t *pte = walk(p->pagetable, clock_va, 0);

    if (!pte || !(*pte & PTE_V))
      goto advance;

    /* Only evict leaf PTEs (those with R, W, or X set) */
    if ((*pte & (PTE_R | PTE_W | PTE_X)) == 0)
      goto advance;

    /* Do not evict executable pages. This avoids instruction stream
     * instability when heavily swapping code pages under test load. */
    if (*pte & PTE_X)
      goto advance;

    /* Only evict user pages */
    if (!(*pte & PTE_U))
      goto advance;

    {
      uint64_t pa = PTE2PA(*pte);

      /* Skip kernel pages (below _kernel_end) */
      if (pa < (uint64_t)_kernel_end)
        goto advance;

      /* Enhanced victim selection using page descriptors */
      struct page *page = virt_to_page((void *)pa);
      if (page) {
        /* Skip pages with multiple mappings (shared pages) */
        int mapcount = atomic_read(&page->mapcount);
        if (mapcount > 0) {
          goto advance;
        }
        
        /* Enhanced aging: use age_score instead of simple accessed bit */
        if (*pte & (1L << 6)) {
          /* Page was accessed - update our tracking */
          mark_page_accessed(page);
          *pte &= ~(1L << 6);
          asm volatile("sfence.vma %0, zero" : : "r"(clock_va));
        }
        
        /* Multi-level aging decision */
        if (page->age_score > 64) {
          /* Young page - give it more chances */
          if (page->age_score > 128) {
            page->age_score -= 32;  /* Rapid aging for very young pages */
          } else {
            page->age_score -= 16;  /* Normal aging */
          }
          mm_stats.clock_second_chance++;
          goto advance;
        }
        
        /* Check access frequency for final decision */
        if (page->access_count > 2) {
          /* Frequently accessed - give one more chance */
          page->access_count >>= 1;  /* Decay access count */
          page->age_score = 32;      /* Reset to moderate age */
          goto advance;
        }
      } else {
        /* Fallback to old reference counting if no page descriptor */
        if (page_ref_get(pa) > 1)
          goto advance;
          
        /* Fallback to simple accessed bit check */
        if (*pte & (1L << 6)) {
          *pte &= ~(1L << 6);
          asm volatile("sfence.vma %0, zero" : : "r"(clock_va));
          goto advance;
        }
      }

      /* This page is the victim -- evict it */
      int slot = swap_alloc();
      if (slot < 0)
        break; /* swap full, stop evicting */
        
      /* Try to unmap from all VMAs using anon_vma reverse mapping */
      int unmapped_count = 0;
      if (page && PageAnon(page)) {
        unmapped_count = try_to_unmap(page);
        if (unmapped_count == 0) {
          /* Could not unmap - manually unmap from current process */
          *pte = PTE_SWAP_ENCODE(slot, PTE_FLAGS(*pte));
          asm volatile("sfence.vma %0, zero" : : "r"(clock_va));
        }
        /* Page should now be unmapped from all processes */
      } else {
        /* Non-anonymous page or no page descriptor - use old method */
        int orig_flags = PTE_FLAGS(*pte);
        *pte = PTE_SWAP_ENCODE(slot, orig_flags);
        asm volatile("sfence.vma %0, zero" : : "r"(clock_va));
      }

      /* Write to swap */
      swap_write(slot, (void *)pa);
      
      /* Update statistics */
      mm_stats.clock_evictions++;
      if (page) {
        /* Mark page as reclaimed in our tracking */
        SetPageReclaim(page);
        printk("swap: evicted page pfn=%lu, unmapped from %d VMAs\n", 
               page_to_pfn(page), unmapped_count);
      }
      
      kfree((void *)pa);
      evicted++;
    }

advance:
    clock_va += PAGE_SIZE;
    if (clock_va >= MAXVA) {
      clock_proc_idx = (clock_proc_idx + 1) % NPROC;
      clock_va = 0;
    }
  }

  return evicted;
}

/* ================================================================== */
/* Debug / stats                                                       */
/* ================================================================== */
void swap_stats(void) {
  printk("swap: %d/%d slots used, enabled=%d\n",
         swap_used_count, SWAP_NSLOTS, swap_is_enabled);
}
