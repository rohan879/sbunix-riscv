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

/* 2MB super page allocation */
#define SUPERPAGE_NPAGES 512   /* number of 4KB pages in one 2MB super page */
void *kalloc_super(void);      /* allocate 2MB-aligned contiguous block */
void  kfree_super(void *pa);   /* free a 2MB contiguous block */

#endif