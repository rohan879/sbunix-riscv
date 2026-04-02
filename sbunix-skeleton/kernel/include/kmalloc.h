#ifndef _KMALLOC_H
#define _KMALLOC_H

#include <riscv.h>

void *kmalloc(uint64_t size);
void  kmfree(void *ptr, uint64_t size);
int   kmalloc_is_managed_page(void *pa);

#endif
