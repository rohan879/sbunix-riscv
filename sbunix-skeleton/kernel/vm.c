#include <kalloc.h>
#include <printk.h>
#include <riscv.h>
#include <string.h>
#include <vm.h>

pagetable_t kernel_pagetable;

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
pte_t *walk(pagetable_t pagetable, uint64_t va, int alloc) {
    for(int level = 2; level > 0; level--) {
        pte_t *pte = &pagetable[PX(level, va)];
        if(*pte & PTE_V) {
            pagetable = (pagetable_t)PTE2PA(*pte);
        } else {
            if(!alloc || (pagetable = (pagetable_t)kalloc()) == 0)
                return 0;
            memset(pagetable, 0, PAGE_SIZE);
            *pte = PA2PTE(pagetable) | PTE_V;
        }
    }
    return &pagetable[PX(0, va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
int mappages(pagetable_t pagetable, uint64_t va, uint64_t size, uint64_t pa, int perm) {
    uint64_t a, last;
    pte_t *pte;

    a = PGROUNDDOWN(va);
    last = PGROUNDDOWN(va + size - 1);
    for(;;) {
        if((pte = walk(pagetable, a, 1)) == 0)
            return -1;
        if(*pte & PTE_V) {
            // Already mapped, but we continue
        }
        *pte = PA2PTE(pa) | perm | PTE_V;
        if(a == last)
            break;
        a += PAGE_SIZE;
        pa += PAGE_SIZE;
    }
    return 0;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(uint64_t va, uint64_t pa, uint64_t sz, int perm) {
    if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
        printk("kvmmap failed\n");
}

// Initialize the one kernel_pagetable
void kvminit(void) {
    kernel_pagetable = (pagetable_t)kalloc();
    memset(kernel_pagetable, 0, PAGE_SIZE);

    // uart registers
    kvmmap(0x10000000, 0x10000000, PAGE_SIZE, PTE_R | PTE_W);

    // PCIe ECAM config space (0x30000000, 256 MiB)
    kvmmap(0x30000000, 0x30000000, 0x10000000, PTE_R | PTE_W);

    // PCIe MMIO BAR window (0x40000000 – 0x80000000, 1 GiB)
    kvmmap(0x40000000, 0x40000000, 0x40000000, PTE_R | PTE_W);

    // PLIC
    kvmmap(0x0c000000, 0x0c000000, 0x400000, PTE_R | PTE_W);
    kvmmap(0x0c200000, 0x0c200000, 0x400000, PTE_R | PTE_W);

    // map kernel text executable and read-only.
    // map kernel data and physical RAM read-write.
    // For simplicity, we map all RAM from 0x80000000 to PHYSTOP
    // with R|W|X permissions. This is not secure but sufficient for now.
    kvmmap(0x80000000, 0x80000000, PHYSTOP - 0x80000000, PTE_R | PTE_W | PTE_X);
    
    // Install the kernel page table (write to satp)
    w_satp(MAKE_SATP(kernel_pagetable));
    sfence_vma(); // flush TLB
    
    printk("kvminit: paging enabled.\n");
}

// Create an empty user page table.
pagetable_t uvmcreate() {
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PAGE_SIZE);
  return pagetable;
}

// Allocate PTEs and physical memory for a virtual address range [start_va, end_va).
// Returns end_va or 0 on error.
uint64_t uvmalloc(pagetable_t pagetable, uint64_t start_va, uint64_t end_va, int xperm) {
  char *mem;
  uint64_t a;

  if(end_va < start_va)
    return start_va;

  start_va = PGROUNDDOWN(start_va);
  for(a = start_va; a < end_va; a += PAGE_SIZE){
    mem = kalloc();
    if(mem == 0){
      return 0;
    }
    memset(mem, 0, PAGE_SIZE);
    if(mappages(pagetable, a, PAGE_SIZE, (uint64_t)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      return 0;
    }
  }
  return end_va;
}

// Map a user virtual address to a physical address
int uvmmap(pagetable_t pagetable, uint64_t va, uint64_t pa, uint64_t sz, int perm) {
    return mappages(pagetable, va, sz, pa, perm | PTE_U);
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int uvmcopyin(pagetable_t pagetable, uint64_t dstva, char *src, uint64_t len) {
  uint64_t n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pte = walk(pagetable, va0, 0);
    if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
      return -1;
    pa0 = PTE2PA(*pte);
    n = PAGE_SIZE - (dstva - va0);
    if(n > len)
      n = len;
    
    // Copy to physical address directly since we are in kernel 1:1 map
    memcpy((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PAGE_SIZE;
  }
  return 0;
}