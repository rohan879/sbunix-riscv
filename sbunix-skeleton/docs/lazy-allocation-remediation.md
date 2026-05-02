# Lazy Allocation Remediation

## User Address Layout

User virtual addresses now occupy `[0x00000000, 0x80000000)`.  The kernel
starts at `KERNBASE`, so `0x80000000` remains exclusive and supervisor-only.
Process page tables own Sv39 root entries 0 and 1; kernel sharing starts at
`KERNBASE`.  Low supervisor MMIO aliases that remain visible in process page
tables are treated as reserved holes, but PCIe MMIO in the kernel page table is
not copied into user roots.

The signal trampoline is fixed at `0x7F000000`, and the user clock page sits one
page below it.  Unhinted `mmap` allocation searches downward below those fixed
pages so large sparse mappings do not consume the contiguous heap window.

## Trap Return

Kernel C runs on `kernel_pagetable`.  A trapframe carries the process `satp`
that `trap_restore` installs only for the final `sret` to user mode.  The
scheduler therefore resumes kernel contexts under the kernel root, and signal
return overwrites any user-supplied `satp` with the process page-table root.

## Sparse Mappings

Anonymous and file-backed VMAs remain metadata-only until a page fault or
copyin/copyout path resolves the page.  A 1 GiB reservation does not require
1 GiB of physical RAM; only the touched pages consume frames.  Fatal user page
faults are delivered as synchronous signals without kernel diagnostic noise,
while kernel and no-process faults still log and stop.

## Shared Mappings

`MAP_SHARED` VMAs attach to a shared mapping registry.  File-backed shared
objects are keyed by stable file identity plus page-aligned file offset.
Anonymous shared mappings allocate an object id and inherit that object through
fork.  Shared pages resolve through registry slots so fork-before-touch and
separate opens observe the same backing page.  Writeback is centralized through
VMA teardown, `msync`, and replacement paths.

## Stack And Heap

The loader records `initial_brk`.  Raw `brk` rejects requests below that point
by returning the unchanged break, and libc reports `ENOMEM` unless the kernel
reaches the requested break.  Stack VMAs are marked explicitly and grow down
only after a full-range preflight checks the 256-page grow window, heap
collision, non-stack VMA collision, and the one-page guard.  Grow failure rolls
back both pages and stack metadata.

## Validation Notes

Focused validation covered sparse reservation, user-top boundaries, lazy
copyin/copyout, signal return, shared mappings, stack/brk guards, mremap,
mprotect, filesystem access, virtio stress, and memory-pressure recovery under
`QEMU_MEM=128M`.  Detailed commands and artifacts are recorded in
`/home/adebayo/test-findings/test-findings-30.md`.
