#ifndef _KALLOC_H
#define _KALLOC_H

#include <riscv.h>

#define PAGE_SIZE 4096

void kinit(void);
void *kalloc(void);
void kfree(void *pa);

/* Page reference counting for COW fork */
void page_ref_inc(uint64_t pa);
void page_ref_dec(uint64_t pa);
int  page_ref_get(uint64_t pa);
void page_ref_set(uint64_t pa, int val);

/* Free page count */
int  get_free_page_count(void);

/* Small contiguous allocation (2-16 pages) for kernel stacks */
void *kalloc_contiguous(int npages);  /* allocate npages contiguous 4KB pages */
void  kfree_contiguous(void *pa, int npages); /* free npages contiguous pages */

/* 2MB super page allocation */
#define SUPERPAGE_NPAGES 512   /* number of 4KB pages in one 2MB super page */
void *kalloc_super(void);      /* allocate 2MB-aligned contiguous block */
void  kfree_super(void *pa);   /* free a 2MB contiguous block */

/* Debug page-frame ownership tracking. */
enum page_frame_type {
  PAGE_UNKNOWN = 0,
  PAGE_FREE,
  PAGE_PAGETABLE,
  PAGE_USER,
  PAGE_SLAB,
  PAGE_DMA,
  PAGE_KERNEL,
};

void page_set_type(uint64_t pa, enum page_frame_type type);
enum page_frame_type page_get_type(uint64_t pa);
const char *page_type_name(enum page_frame_type type);
void page_mark_range(uint64_t start, uint64_t end, enum page_frame_type type);

/* Page table page tracking for debugging */
void page_mark_ptpage(uint64_t pa);
int  page_is_ptpage(uint64_t pa);

/* Process table exhaustion tracking */
void track_proc_alloc(int current_count);
void track_proc_exhaustion(void);
int  get_proc_highwater(void);
int  get_proc_exhaustion_count(void);

#endif
