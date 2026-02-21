#ifndef _VM_H
#define _VM_H

#include <riscv.h>

void kvminit(void);
void kvmmap(uint64_t va, uint64_t pa, uint64_t sz, int perm);
int mappages(pagetable_t pagetable, uint64_t va, uint64_t size, uint64_t pa, int perm);
pte_t *walk(pagetable_t pagetable, uint64_t va, int alloc);
pagetable_t uvmcreate(void);
void uvmfirst(pagetable_t pagetable, unsigned char *src, uint32_t sz);
uint64_t uvmalloc(pagetable_t pagetable, uint64_t start_va, uint64_t end_va, int xperm);
int uvmmap(pagetable_t pagetable, uint64_t va, uint64_t pa, uint64_t sz, int perm);
int uvmcopyin(pagetable_t pagetable, uint64_t dstva, char *src, uint64_t len);
void uvminit(pagetable_t pagetable, unsigned char *init, uint32_t sz); // Load initial user program

#endif
