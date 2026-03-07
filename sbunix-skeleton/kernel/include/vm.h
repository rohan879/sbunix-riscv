#ifndef _VM_H
#define _VM_H

#include <riscv.h>

extern pagetable_t kernel_pagetable;

void kvminit(void);
void kvmmap(uint64_t va, uint64_t pa, uint64_t sz, int perm);
int mappages(pagetable_t pagetable, uint64_t va, uint64_t size, uint64_t pa, int perm);
pte_t *walk(pagetable_t pagetable, uint64_t va, int alloc);
pagetable_t uvmcreate(void);
uint64_t uvmalloc(pagetable_t pagetable, uint64_t start_va, uint64_t end_va, int xperm);
int uvmmap(pagetable_t pagetable, uint64_t va, uint64_t pa, uint64_t sz, int perm);
void uvmunmap(pagetable_t pagetable, uint64_t va, uint64_t len);
void uvmmap_sigtrampoline(pagetable_t pagetable);
int uvmcopyin(pagetable_t pagetable, uint64_t dstva, char *src, uint64_t len);

#endif
