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

#endif