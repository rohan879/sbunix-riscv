#ifndef _VM_H
#define _VM_H

#include <riscv.h>
#include <mm.h>

/* Protection bits for vm_area (must match libc PROT_* values) */
#define PROT_READ   0x01
#define PROT_WRITE  0x02
#define PROT_EXEC   0x04

/* Mapping flags for vm_area (must match libc MAP_* values) */
#define MAP_PRIVATE   0x02
#define MAP_ANON      0x20

/* Virtual memory area descriptor */
struct open_file; /* forward declaration */

struct vm_area {
  uint64_t start;       /* page-aligned start address */
  uint64_t end;         /* page-aligned end address (exclusive) */
  int prot;             /* PROT_READ | PROT_WRITE | PROT_EXEC */
  int flags;            /* MAP_PRIVATE | MAP_ANONYMOUS */
  struct vm_area *next; /* sorted linked list by start addr */
  
  /* File-backed mapping (NULL for anonymous) */
  struct open_file *file; /* backing file (refcount'd) */
  uint64_t file_offset;   /* offset within file for start of mapping */
  
  /* Anonymous VMA reverse mapping support */
  struct list_head anon_vma_chain;   /* list of anon_vma_chain entries */
  struct anon_vma *anon_vma;         /* primary anon_vma (optimization) */
  
  /* Enhanced VMA statistics */
  uint64_t vm_pgoff;            /* offset within anon_vma (pages) */
  uint64_t resident_pages;      /* number of resident pages in this VMA */
};

struct proc; /* forward declaration */

int vma_add(struct proc *p, uint64_t start, uint64_t end, int prot, int flags);
struct vm_area *vma_find(struct proc *p, uint64_t addr);
int vma_remove(struct proc *p, uint64_t start, uint64_t end);
int vma_dup(struct proc *src, struct proc *dst);
void vma_free_all(struct proc *p);

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
void proc_freepagetable(pagetable_t pagetable);
int uvmmap_kernel(pagetable_t pt);
int copyout(pagetable_t pagetable, uint64_t dstva, void *src, uint64_t len);
int copyin(pagetable_t pagetable, void *dst, uint64_t srcva, uint64_t len);
int copyinstr(pagetable_t pagetable, char *dst, uint64_t srcva, uint64_t max);

/* Recursive paging: check/repair kernel code PTEs via the gigapage
 * window at L2[510], bypassing the identity-mapped superpages that
 * are susceptible to DMA-induced PTE_X corruption. */

/* Two-step L1 PTE access through the self-referencing gigapage window.
 * Step 1: Read the L2 entry via the window to find the L1 table PA.
 * Step 2: Access the L1 PTE through the window.
 * This does NOT call walk() and does NOT depend on the identity-mapped
 * L1 superpages (which are the ones losing PTE_X).
 * Defined inline in the header so both vm.c and trap.c can use it. */
static inline volatile pte_t *recursive_l1_pte(uint64_t va) {
  /* Step 1: read L2 entry for VPN[2] of va */
  volatile pte_t *l2_ptr =
      (volatile pte_t *)RECURSIVE_L2_PTE_VA(PX(2, va));
  pte_t l2_entry = *l2_ptr;
  if (!(l2_entry & PTE_V)) return 0;
  /* Step 2: compute L1 PTE address through the window */
  uint64_t l1_table_pa = PTE2PA(l2_entry);
  return (volatile pte_t *)RECURSIVE_PA2VA(
      l1_table_pa + PX(1, va) * sizeof(pte_t));
}

int recursive_check_kcode_pte(void);
int recursive_check_all_kernel_ptes(void);

/* BF: Centralized COW break — ref count, page copy, PTE update, rmap */
int cow_break(pagetable_t pagetable, pte_t *pte, uint64_t va,
              struct vm_area *vma);

#endif
