#ifndef _SWAP_H
#define _SWAP_H

#include <riscv.h>
#include <kalloc.h>

/* ------------------------------------------------------------------ */
/* Swap space constants                                                */
/* ------------------------------------------------------------------ */
#define SWAP_NSLOTS       1024              /* 1024 pages = 4 MB of swap */
#define SECTORS_PER_PAGE  (PAGE_SIZE / 512) /* 8 sectors per 4 KB page   */
#define SWAP_NSECTORS     (SWAP_NSLOTS * SECTORS_PER_PAGE) /* 8192 sectors */

/* Eviction thresholds */
#define SWAP_LOW_WATERMARK  32  /* trigger eviction when free pages below this */
#define SWAP_EVICT_BATCH     8  /* evict this many pages per trigger */

/* ------------------------------------------------------------------ */
/* PTE encoding for swapped-out pages                                  */
/*                                                                      */
/* When PTE_V=0, hardware ignores all other bits.  We repurpose them:  */
/*   Bit  0:     0           (PTE_V cleared -- triggers page fault)    */
/*   Bit  8:     1           (PTE_SWAP marker, same position as PTE_COW)*/
/*   Bits 1-7:   original permission flags (R,W,X,U,G,A,D) preserved   */
/*   Bits 10-19: swap slot index (10 bits for 1024 slots)              */
/*   Bits 54-60: fixed magic tag to distinguish swap from junk PTEs    */
/*                                                                      */
/* The additional magic tag is important during teardown: stale or      */
/* corrupted invalid PTE values must not be mistaken for swapped pages. */
/* ------------------------------------------------------------------ */
#define PTE_SWAP              (1L << 8)
#define PTE_SWAP_SLOT_SHIFT   10
#define PTE_SWAP_SLOT_MASK    (((uint64_t)(SWAP_NSLOTS - 1)) << PTE_SWAP_SLOT_SHIFT)
#define PTE_SWAP_MAGIC_SHIFT  54
#define PTE_SWAP_MAGIC_MASK   (0x7fULL << PTE_SWAP_MAGIC_SHIFT)
#define PTE_SWAP_MAGIC        (0x2dULL << PTE_SWAP_MAGIC_SHIFT)

/* Encode a swapped-out PTE.  Store original flags (excluding V) and slot. */
#define PTE_SWAP_ENCODE(slot, orig_flags) \
  (PTE_SWAP_MAGIC | (((uint64_t)(slot) & (SWAP_NSLOTS - 1)) << PTE_SWAP_SLOT_SHIFT) | \
   PTE_SWAP | ((orig_flags) & 0xFE))

/* Decode swap slot from a swapped-out PTE */
#define PTE_SWAP_SLOT(pte)  ((int)(((pte) & PTE_SWAP_SLOT_MASK) >> PTE_SWAP_SLOT_SHIFT))

/* Recover original permission flags for remapping on swap-in */
#define PTE_SWAP_FLAGS(pte) ((int)((pte) & 0xFE))

/* Test whether a PTE represents a swapped-out page */
#define PTE_IS_SWAPPED(pte) \
  ((((pte) & PTE_V) == 0) && \
   (((pte) & (PTE_SWAP | PTE_SWAP_MAGIC_MASK)) == (PTE_SWAP | PTE_SWAP_MAGIC)) && \
   ((PTE_SWAP_FLAGS(pte) & PTE_U) != 0) && \
   (PTE_SWAP_SLOT(pte) >= 0) && (PTE_SWAP_SLOT(pte) < SWAP_NSLOTS))

/* Take ownership of a swapped-out PTE: return its slot, optionally its flags,
 * and clear the PTE so the slot cannot be released twice by another teardown
 * path. Returns -1 if the PTE is not swapped out. */
static inline int pte_swap_take(pte_t *pte, int *flags_out) {
  pte_t old = *pte;
  if (!PTE_IS_SWAPPED(old))
    return -1;
  if (flags_out)
    *flags_out = PTE_SWAP_FLAGS(old);
  *pte = 0;
  return PTE_SWAP_SLOT(old);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
void swap_init(void);
int  swap_alloc(void);
void swap_free(int slot);
void swap_write(int slot, void *page);
void swap_read(int slot, void *page);
void swap_evict_pages(int n);
void swap_stats(void);

/* Returns 1 if swap subsystem is enabled (disk present and large enough) */
int  swap_enabled(void);

#endif
