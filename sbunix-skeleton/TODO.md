# SBUnix TODO — RISC-V

## Warm-up Project 1 (Part 1): Shell (sbush) — DONE

- [x] `bin/sbush`: interactive shell binary (cross-compiled for RV64)
- [x] Change directory (`cd /path`)
- [x] Execute binaries (search PATH)
- [x] Execute scripts (files starting with `#!sbush`)
- [x] Set and use `PATH` and `PS1` environment variables
- [x] Background processes (`ls &`)
- [x] Pipes (`cat /path | grep word`)
- [x] Testing: QEMU user-mode (`qemu-riscv64`) or inside kernel once exec/ELF loading works

## Warm-up Project 1 (Part 2): Standard Library (sblibc) — DONE

- [x] Syscall wrappers using RISC-V Linux ABI (number in `a7`, args `a0`-`a5`, `ecall`, return in `a0`)
  - Syscall table: https://gpages.juszkiewicz.com.pl/syscalls-table/syscalls.html
  - Use `clone` (not fork), `openat` (not open), `pipe2` (not pipe), `dup3` (not dup2)
- [x] Implement all libc functions needed by sbush
- [x] `libc/crt.S`: proper C runtime startup (set up argc/argv, call main, call exit)
- [x] `bin/ls` and `bin/cat` as test binaries (compiled for RV64)

## Warm-up Project 2 (Part 1): Kernel printf() — DONE

- [x] `printk` with `%c %d %x %s %p` format specifiers
- [x] Output via NS16550 UART at `0x10000000`

## Warm-up Project 2 (Part 2): Timer Interrupt — DONE

- [x] Set up `stvec` trap vector and trap handler dispatching on `scause`
- [x] Timer interrupt via SBI timer extension (`sbi_set_timer`)
- [x] Track time since boot
- [x] Display time since boot at top-right of console (row 1, col 60) using VT100 escapes

## Warm-up Project 2 (Part 3): Keyboard Interrupt — DONE

- [x] UART RX interrupt via PLIC (no PS/2 on RISC-V virt; keyboard input arrives as UART bytes)
- [x] Display last pressed glyph next to clock (row 1, col 72)
- [x] Handle Shift (terminal sends shifted ASCII — displayed directly)
- [x] Handle Control key (show as `^C`, `^A`, etc.; caret notation for 0x00-0x1F and 0x7F)
- [x] No local echo (interrupt handler only updates status bar, does not echo to main output)

## Bugs / Issues

- BUG: Kernel syscall numbers (`kernel/include/syscall.h`) do not match libc's Linux RISC-V ABI numbers (`libc/include/syscall.h`). User processes send Linux numbers (e.g. write=64, read=63) but kernel expects custom numbers (write=1, read=3), causing "Unknown syscall" at runtime.

## Warm-up Project 3 (Part 1): Device Discovery

- [ ] Parse DTB (passed in `a1` at boot) to enumerate devices
  - UART, PLIC, CLINT, virtio-mmio regions, PCIe host bridge
  - RISC-V equivalent of x86 PCI config space walk
- [ ] Locate the virtio-blk block device

## Warm-up Project 3 (Part 2): Block Device (virtio-blk)

- [ ] virtio-blk driver over virtio-mmio (replaces AHCI on x86)
- [ ] Write 100 x 4KB blocks (block N filled with byte N)
- [ ] Read back and verify correctness

## Final Project — Plan of Attack

### 1. Boot + Early Console (partially done)

- [x] OpenSBI handoff, UART printing
- [ ] Parse DTB pointer from `a1`
- [x] Set `stvec` trap vector

### 2. Trap/Interrupt Subsystem

- [x] Exception entry/exit (`sepc`/`scause`/`stval` dispatch)
- [x] Timer interrupts (SBI)
- [x] External interrupts via PLIC
- [ ] Minimal scheduler tick for preemption

### 3. Memory Subsystem

- [ ] Physical page allocator (page descriptors, free list)
- [ ] Sv39 page tables (3-level, managed via `satp` CSR)
- [ ] Enable/disable virtual memory
- [ ] `kmalloc`

### 4. Process Subsystem

- [ ] Kernel threads
- [ ] Context switch (RISC-V register save/restore)
- [ ] User-mode entry (U-mode), per-process page tables

### 5. User-level Subsystem

- [ ] VMAs / vm_map_entry tracking
- [ ] COW fork (via `clone`), auto-growing stack, graceful SEGV
- [ ] Page fault handler (`scause` exception codes)

### 6. I/O Subsystem

- [ ] Syscall dispatch (`ecall` → `scause` environment-call-from-U-mode)
- [ ] `read(stdin)`, `write(stdout)`, `write(stderr)` via UART terminal
- [ ] VFS layer
- [ ] tarfs: `open`, `read`, `close`, `opendir`, `readdir`, `closedir`
  - Embedded in `.tarfs` section (`_tarfs_start` / `_tarfs_end`)

### 7. Userspace Binaries

- [ ] `bin/echo`, `bin/sleep`, `bin/cat`, `bin/ls`
- [ ] `bin/kill` (`kill -9`), `bin/ps`
- [ ] `bin/sbush` (full shell)

### 8. /bin/init

- [ ] fork(clone) + exec + wait on `/etc/rc`
- [ ] Then exec sbush

## Grading Targets

- [ ] Cooperative multi-tasking + all functional requirements (50 pts)
- [ ] Preemptive multi-tasking (+10 pts)
- [ ] Signals (INT on ^C, ALARM, SEGV) and pipes (+10 pts)
- [ ] On-disk r/w file system (+10 pts)
- [ ] On-disk swap and paging (+10 pts)
- [ ] TCP/IP networking via lwIP (+10 pts)
- [ ] Multi-core (+20 pts)
