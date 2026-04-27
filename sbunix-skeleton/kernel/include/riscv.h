#ifndef _RISCV_H
#define _RISCV_H

// Manual typedefs for fixed-width integers
typedef unsigned long  uint64_t;
typedef unsigned int   uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;
typedef long           int64_t;
typedef int            int32_t;
typedef unsigned long  size_t;
typedef long           ssize_t;
typedef int            pid_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

// For qemu virt machine, physical memory starts at 0x80000000.
#define KERNBASE 0x80000000UL
/* BF: Keep kernel PHYSTOP in sync with QEMU_MEM=256M so large COW
 * pressure tests exercise ownership instead of the old 128MB ceiling. */
#define PHYSMEM_SIZE (256UL * 1024 * 1024)
#define PHYSTOP (KERNBASE + PHYSMEM_SIZE)

// Round a value up to the next multiple of b
#define ROUNDUP(a, b) ((((a)-1)/(b) + 1) * (b))
#define PGROUNDUP(sz)  (((sz)+PAGE_SIZE-1) & ~(PAGE_SIZE-1))
#define PGROUNDDOWN(a) (((a)) & ~(PAGE_SIZE-1))

/* 2MB super page (megapage) support for Sv39 level-1 leaf PTEs */
#define SUPERPAGE_SIZE   (512 * PAGE_SIZE)           /* 2 MB = 0x200000 */
#define SUPERPAGE_SHIFT  21
#define SUPERPAGE_MASK   (SUPERPAGE_SIZE - 1)        /* 0x1FFFFF */
#define SPGROUNDDOWN(a)  ((a) & ~(uint64_t)SUPERPAGE_MASK)
#define SPGROUNDUP(a)    (((a) + SUPERPAGE_MASK) & ~(uint64_t)SUPERPAGE_MASK)
#define IS_SUPERPAGE_ALIGNED(a)  (((uint64_t)(a) & SUPERPAGE_MASK) == 0)

/* A PTE is a leaf if any of R, W, X is set (Sv39 spec) */
#define PTE_IS_LEAF(pte)  ((pte) & (PTE_R | PTE_W | PTE_X))

// PTE flags
#define PTE_V (1L << 0) // valid
#define PTE_R (1L << 1)
#define PTE_W (1L << 2)
#define PTE_X (1L << 3)
#define PTE_U (1L << 4) // user can access
#define PTE_A (1L << 6) // accessed
#define PTE_D (1L << 7) // dirty
#define PTE_COW (1L << 8) // RSW bit 0: copy-on-write marker

// Shift a physical address to the right place for a PTE.
#define PA2PTE(pa) ((((uint64_t)pa) >> 12) << 10)
#define PTE2PA(pte) (((pte) >> 10) << 12)
#define PTE_FLAGS(pte) ((pte) & 0x3FF)

// Extract the 3 9-bit page table indices from a virtual address.
#define PXMASK          0x1FF // 9 bits
#define PXSHIFT(level)  (12 + (level) * 9)
#define PX(level, va)   ((((uint64_t)(va)) >> PXSHIFT(level)) & PXMASK)

/* ---- Sv39 self-referencing page table window ---- */
/* A 1GB leaf gigapage at L2 index 510 maps all of kernel RAM through
 * a dedicated VA window.  Unlike x86, RISC-V Sv39 determines leaf
 * vs. non-leaf by R/W/X bits at ALL levels (including L0), so
 * x86-style multi-level recursive MMU walks don't work.  Instead,
 * the gigapage gives the MMU a one-step translation, and software
 * computes PTE offsets within the window.
 *
 * Purpose: provide a read-only diagnostic alias for physical RAM and
 * page-table inspection without creating a writable alias of executable
 * kernel text. */
#define RECURSIVE_INDEX   510
#define RECURSIVE_BASE    0xFFFFFFFF80000000UL  /* VPN[2]=510, sign-extended */
#define PHYS_RAM_BASE     0x80000000UL

/* Convert a physical address (in kernel RAM) to a VA in the
 * self-referencing gigapage window.  Works for any PA in
 * [0x80000000, 0xC0000000). */
#define RECURSIVE_PA2VA(pa) \
    (RECURSIVE_BASE + ((uint64_t)(pa) - PHYS_RAM_BASE))

/* Read an L2 (root) PTE for the given VPN[2] index.
 * Single-step: kernel_pagetable is at a known PA. */
#define RECURSIVE_L2_PTE_VA(vpn2) \
    RECURSIVE_PA2VA((uint64_t)kernel_pagetable + (vpn2) * sizeof(pte_t))

// User address space boundary.
// Set to 1GB to stay safely below kernel space (0x80000000).
#define MAXVA 0x40000000L
#define USER_TOP MAXVA

static inline int user_addr_ok(uint64_t va) {
    return va < USER_TOP;
}

static inline int user_range_ok(uint64_t va, uint64_t len) {
    if (len == 0)
        return va < USER_TOP;
    if (va >= USER_TOP)
        return 0;
    if (len > USER_TOP - va)
        return 0;
    return 1;
}

#define USER_STACK_TOP  MAXVA   /* user stack grows down from top of VA space */

/* BF: Per-process sigtrampoline page VA — executes rt_sigreturn when
 * no sa_restorer is set.  In L1 index 252 (0x3F000000 >> 21 = 504 >> 2),
 * above uvmmap_kernel share_limit (129), well below user stack. */
#define SIGTRAMP_VA     0x3F000000UL

/* BF: Floor for user mmap allocations — above all shared kernel MMIO L1
 * entries.  L1[128] = UART at 0x10000000 is the highest safe device.
 * L1[129] starts at 0x10200000, so mmap allocations begin here.
 * PCIe ECAM at L1[384-510] (0x30000000+) is excluded from user page
 * tables entirely by the uvmmap_kernel() share limit. */
#define USER_MMAP_FLOOR 0x10200000UL

typedef uint64_t pte_t;
typedef uint64_t *pagetable_t; // 512 PTEs

// use riscv's sv39 page table scheme.
#define SATP_SV39 (8L << 60)

#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64_t)pagetable) >> 12))

#define SSTATUS_SPP (1L << 8)  // Previous mode, 1=Supervisor, 0=User
#define SSTATUS_SPIE (1L << 5) // Supervisor Previous Interrupt Enable
#define SSTATUS_SUM (1L << 18) // Supervisor User Memory access
#define SSTATUS_SIE  (1L << 1)

#define SIE_SSIE (1L << 1)
#define SIE_STIE (1L << 5)
#define SIE_SEIE (1L << 9)

#define SCAUSE_INTERRUPT      (1UL << 63)
#define SCAUSE_S_TIMER_INT    (SCAUSE_INTERRUPT | 5)
#define SCAUSE_S_EXTERNAL_INT (SCAUSE_INTERRUPT | 9)
#define SCAUSE_ECALL_U        8
#define SCAUSE_INST_PAGE_FAULT   12
#define SCAUSE_LOAD_PAGE_FAULT   13
#define SCAUSE_STORE_PAGE_FAULT  15

static inline unsigned long r_sie() {
    unsigned long x;
    asm volatile("csrr %0, sie" : "=r" (x));
    return x;
}

static inline void w_sie(unsigned long x) {
    asm volatile("csrw sie, %0" : : "r" (x));
}

static inline unsigned long r_time() {
    unsigned long x;
    asm volatile("rdtime %0" : "=r" (x));
    return x;
}

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

static inline unsigned long r_sepc() {
    unsigned long x;
    asm volatile("csrr %0, sepc" : "=r" (x) );
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

// Read the SATP register
static inline unsigned long r_satp() {
    unsigned long x;
    asm volatile("csrr %0, satp" : "=r" (x));
    return x;
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
