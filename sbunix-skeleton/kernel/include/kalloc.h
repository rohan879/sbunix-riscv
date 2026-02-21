#ifndef _KALLOC_H
#define _KALLOC_H

#define PAGE_SIZE 4096

void kinit(void);
void *kalloc(void);
void kfree(void *pa);

#endif