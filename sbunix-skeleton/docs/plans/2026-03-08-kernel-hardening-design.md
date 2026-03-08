# Kernel Hardening & Refactoring Design

## Overview

Three features addressing a security bug, a stub implementation, and code
maintainability in the SBUnix RISC-V kernel.

**Priority order**: copyin/copyout (security) → dispatch table (refactor) →
mmap/munmap/mprotect (completion).

---

## Feature 1: copyin/copyout/copyinstr (Security Fix)

**Problem**: Syscall handler in trap.c directly dereferences user pointers
without validation. 10+ syscalls cast `tf->aN` to pointers and use them
unsafely (strlen, memcpy, direct write). A malicious program can pass
kernel addresses to corrupt kernel memory.

**Solution**: Three validation functions in vm.c:

```c
int copyin(pagetable_t pt, void *dst, uint64_t srcva, uint64_t len);
int copyout(pagetable_t pt, uint64_t dstva, void *src, uint64_t len);
int copyinstr(pagetable_t pt, char *dst, uint64_t srcva, uint64_t max);
```

**Validation logic** (all three):
1. Reject if VA >= MAXVA (0x40000000)
2. walk_super() to get PTE and level
3. Check PTE_V | PTE_U (copyout also checks PTE_W)
4. Copy up to page boundary, advance, repeat
5. Return 0 on success, -1 on validation failure

copyinstr stops at null terminator, returns -1 if max exhausted.

**Refactoring**: Every unsafe syscall gets a kernel-side buffer:
- Paths: `char kpath[256]; copyinstr(..., tf->a1, 256)`
- Structs: `uint8_t kstat[128]; copyout(...)`
- Small values: `int kfds[2]; copyout(...)`

The ~12 syscalls already using inline walk() get converted too.

Existing `uvmcopyin()` renamed to `copyout()` (it copies kernel→user).

---

## Feature 2: Syscall Dispatch Table (Refactor)

**Problem**: trap_handler() contains a 1,390-line if-else chain with 56
branches (lines 911-2303). Unmaintainable.

**Solution**: Extract each branch into a static function:

```c
static long sys_brk(struct trapframe *tf);
static long sys_write(struct trapframe *tf);
// ... 54 more
```

Dense 501-entry function pointer table (covers SYS_getcwd=17 through
SYS_ps=500):

```c
#define NSYSCALLS 501
static long (*syscall_table[NSYSCALLS])(struct trapframe *tf);
```

Populated via `syscall_init()` called from boot().

Dispatch replaces if-else:

```c
if (sysnum >= 0 && sysnum < NSYSCALLS && syscall_table[sysnum])
  tf->a0 = syscall_table[sysnum](tf);
```

**Special cases** (clone, exit, execve, sigreturn) return sentinel
`SYSCALL_NORETURN` to skip tf->a0 assignment.

Helper functions stay in trap.c above syscall functions.

---

## Feature 3: Complete mmap/munmap/mprotect

**3a. mmap prot handling**: Replace hardcoded PTE_W with prot-to-PTE
translation. PROT_READ→PTE_R, PROT_WRITE→PTE_W, PROT_EXEC→PTE_X.
Modify uvmalloc() to accept caller-provided permission flags.

**3b. munmap multi-VMA**: Replace single vma_find() with a loop walking
VMA list, unmapping each overlapping region. vma_remove() already handles
splits.

**3c. mprotect implementation**: Replace 2-line stub with:
1. Validate addr alignment, len > 0, addr + len <= MAXVA
2. Walk each page via walk(), update PTE R/W/X bits
3. Split super pages if encountered (superpage_split())
4. Split and re-add VMA with new prot
5. sfence_vma() to flush TLB

---

## Constraints

- Single-core RISC-V (no locking needed)
- Must compile with -Wall -Werror
- Must not break existing functionality
- Cross-compiler: riscv64-unknown-elf-gcc
- Test by booting in QEMU: make qemu
