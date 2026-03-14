#ifndef _VM_H
#define _VM_H

#include <riscv.h>

/* Protection bits for vm_area (must match libc PROT_* values) */
#define PROT_READ   0x01
#define PROT_WRITE  0x02
#define PROT_EXEC   0x04

/* Mapping flags for vm_area (must match libc MAP_* values) */
#define MAP_PRIVATE   0x02
#define MAP_ANON      0x20

/* Virtual memory area descriptor */
struct vm_area {
  uint64_t start;       /* page-aligned start address */
  uint64_t end;         /* page-aligned end address (exclusive) */
  int prot;             /* PROT_READ | PROT_WRITE | PROT_EXEC */
  int flags;            /* MAP_PRIVATE | MAP_ANONYMOUS */
  struct vm_area *next; /* sorted linked list by start addr */
};

struct proc; /* forward declaration */

int vma_add(struct proc *p, uint64_t start, uint64_t end, int prot, int flags);
int vma_add_list(struct vm_area **head, uint64_t start, uint64_t end, int prot,
                 int flags);
struct vm_area *vma_find(struct proc *p, uint64_t addr);
int vma_remove(struct proc *p, uint64_t start, uint64_t end);
int vma_dup(struct proc *src, struct proc *dst);
void vma_free_all(struct proc *p);
void vma_free_list(struct vm_area **head);

extern pagetable_t kernel_pagetable;

void kvminit(void);
void kvmmap(uint64_t va, uint64_t pa, uint64_t sz, int perm);
int mappages(pagetable_t pagetable, uint64_t va, uint64_t size, uint64_t pa, int perm);
pte_t *walk(pagetable_t pagetable, uint64_t va, int alloc);

/* 2MB super page support */
int  mappages_super(pagetable_t pagetable, uint64_t va, uint64_t size,
                    uint64_t pa, int perm);
pte_t *walk_super(pagetable_t pagetable, uint64_t va, int *level_out);
void  superpage_split(pagetable_t pagetable, uint64_t va);
void  kvmmap_super(uint64_t va, uint64_t pa, uint64_t sz, int perm);
pagetable_t uvmcreate(void);
uint64_t uvmalloc(pagetable_t pagetable, uint64_t start_va, uint64_t end_va, int perm);
int uvmmap(pagetable_t pagetable, uint64_t va, uint64_t pa, uint64_t sz, int perm);
void uvmunmap(pagetable_t pagetable, uint64_t va, uint64_t len);
void freewalk_user(pagetable_t pagetable);
void uvmprotect(pagetable_t pagetable, uint64_t va, uint64_t len, int perm);
void uvmmap_sigtrampoline(pagetable_t pagetable);
int copyout(pagetable_t pagetable, uint64_t dstva, void *src, uint64_t len);
int copyin(pagetable_t pagetable, void *dst, uint64_t srcva, uint64_t len);
int copyinstr(pagetable_t pagetable, char *dst, uint64_t srcva, uint64_t max);

#endif
