#ifndef _RISCV_H
#define _RISCV_H

// Manual typedefs for fixed-width integers
typedef unsigned long  uint64_t;
typedef unsigned int   uint32_t;
typedef unsigned short uint16_t;
typedef long           int64_t;
typedef int            int32_t;
typedef unsigned long  size_t;

// For qemu virt machine, physical memory starts at 0x80000000 and is 128MB.
#define PHYSTOP 0x88000000

// Round a value up to the next multiple of b
#define ROUNDUP(a, b) ((((a)-1)/(b) + 1) * (b))
#define PGROUNDUP(sz)  (((sz)+PAGE_SIZE-1) & ~(PAGE_SIZE-1))
#define PGROUNDDOWN(a) (((a)) & ~(PAGE_SIZE-1))

// PTE flags
#define PTE_V (1L << 0) // valid
#define PTE_R (1L << 1)
#define PTE_W (1L << 2)
#define PTE_X (1L << 3)
#define PTE_U (1L << 4) // user can access

// Shift a physical address to the right place for a PTE.
#define PA2PTE(pa) ((((uint64_t)pa) >> 12) << 10)
#define PTE2PA(pte) (((pte) >> 10) << 12)
#define PTE_FLAGS(pte) ((pte) & 0x3FF)

// Extract the 3 9-bit page table indices from a virtual address.
#define PXMASK          0x1FF // 9 bits
#define PXSHIFT(level)  (12 + (level) * 9)
#define PX(level, va)   ((((uint64_t)(va)) >> PXSHIFT(level)) & PXMASK)

// User address space boundary. 
// Set to 1GB to stay safely below kernel space (0x80000000).
#define MAXVA 0x40000000L

typedef uint64_t pte_t;
typedef uint64_t *pagetable_t; // 512 PTEs

// use riscv's sv39 page table scheme.
#define SATP_SV39 (8L << 60)

#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64_t)pagetable) >> 12))

#define SSTATUS_SPP (1L << 8)  // Previous mode, 1=Supervisor, 0=User
#define SSTATUS_SPIE (1L << 5) // Supervisor Previous Interrupt Enable
#define SSTATUS_SUM (1L << 18) // Supervisor User Memory access

static inline unsigned long r_scause() {
    unsigned long x;
    asm volatile("csrr %0, scause" : "=r" (x) );
    return x;
}

static inline unsigned long r_stval() {
    unsigned long x;
    asm volatile("csrr %0, stval" : "=r" (x) );
    return x;
}

static inline unsigned long r_sstatus() {
    unsigned long x;
    asm volatile("csrr %0, sstatus" : "=r" (x) );
    return x;
}

static inline void w_sstatus(unsigned long x) {
    asm volatile("csrw sstatus, %0" : : "r" (x));
}

// Write the SATP register to enable paging
static inline void w_satp(unsigned long x) {
    asm volatile("csrw satp, %0" : : "r" (x));
}

// Flush the TLB
static inline void sfence_vma() {
    asm volatile("sfence.vma zero, zero");
}

#endif