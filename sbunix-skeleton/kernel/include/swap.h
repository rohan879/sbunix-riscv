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
/*   Bit  0:     0         (PTE_V cleared -- triggers page fault)      */
/*   Bit  8:     1         (PTE_SWAP marker)                           */
/*   Bit  9:     1 if page was COW-marked (PTE_COW) before eviction   */
/*   Bits 1-7:   original permission flags (R,W,X,U,G,A,D) preserved  */
/*   Bits 10-53: swap slot index (44 bits, only 10 needed for 1024)    */
/*                                                                      */
/* PTE_SWAP uses bit 8 as the swap-present marker.  Bit 9 (RSW bit 1) */
/* preserves the PTE_COW flag so that swap-in can restore COW          */
/* semantics and prevent write-through to shared pages.                */
/* ------------------------------------------------------------------ */
#define PTE_SWAP      (1L << 8)
#define PTE_SWAP_COW  (1L << 9)  /* COW flag preserved across swap */

/* Encode a swapped-out PTE.  Store original flags (excluding V) and slot.
 * If orig_flags has PTE_COW (bit 8), store it in bit 9 so it survives
 * the round-trip through swap. */
#define PTE_SWAP_ENCODE(slot, orig_flags) \
  (((uint64_t)(slot) << 10) | PTE_SWAP | ((orig_flags) & 0xFE) | \
   (((orig_flags) & PTE_COW) ? PTE_SWAP_COW : 0))

/* Decode swap slot from a swapped-out PTE */
#define PTE_SWAP_SLOT(pte)  ((int)((pte) >> 10))

/* Recover original permission flags for remapping on swap-in.
 * Restores PTE_COW (bit 8) from PTE_SWAP_COW (bit 9). */
#define PTE_SWAP_FLAGS(pte) \
  ((int)(((pte) & 0xFE) | (((pte) & PTE_SWAP_COW) ? PTE_COW : 0)))

/* Test whether a PTE represents a swapped-out page */
#define PTE_IS_SWAPPED(pte) (((pte) & PTE_V) == 0 && ((pte) & PTE_SWAP))

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
