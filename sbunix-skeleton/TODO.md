# SBUnix TODO — RISC-V

## Warm-up Project 1 (Part 1): Shell (sbush)

- [ ] `bin/sbush`: interactive shell binary (cross-compiled for RV64)
- [ ] Change directory (`cd /path`)
- [ ] Execute binaries (search PATH)
- [ ] Execute scripts (files starting with `#!sbush`)
- [ ] Set and use `PATH` and `PS1` environment variables
- [ ] Background processes (`ls &`)
- [ ] Pipes (`cat /path | grep word`)
- [ ] Testing: QEMU user-mode (`qemu-riscv64`) or inside kernel once exec/ELF loading works

## Warm-up Project 1 (Part 2): Standard Library (sblibc)

- [ ] Syscall wrappers using RISC-V Linux ABI (number in `a7`, args `a0`-`a5`, `ecall`, return in `a0`)
  - Syscall table: https://gpages.juszkiewicz.com.pl/syscalls-table/syscalls.html
  - Use `clone` (not fork), `openat` (not open), `pipe2` (not pipe), `dup3` (not dup2)
- [ ] Implement all libc functions needed by sbush
- [ ] `libc/crt.S`: proper C runtime startup (set up argc/argv, call main, call exit)
- [ ] `bin/ls` and `bin/cat` as test binaries (compiled for RV64)

## Warm-up Project 2 (Part 1): Kernel printf() — DONE

- [x] `printk` with `%c %d %x %s %p` format specifiers
- [x] Output via NS16550 UART at `0x10000000`

## Warm-up Project 2 (Part 2): Timer Interrupt

- [ ] Set up `stvec` trap vector and trap handler dispatching on `scause`
- [ ] Timer interrupt via CLINT or SBI timer extension (`sbi_set_timer`)
- [ ] Track time since boot
- [ ] Display time since boot in lower-right corner of console
- [ ] (Optional) Read RTC for real-world time

## Warm-up Project 2 (Part 3): Keyboard Interrupt

- [ ] UART RX interrupt via PLIC (no PS/2 on RISC-V virt; keyboard input arrives as UART bytes)
- [ ] Display last pressed glyph next to clock
- [ ] Handle Shift (interpret ASCII bytes from UART)
- [ ] Handle Control key (show as `^C`; byte `0x03` etc.)
- [ ] No local echo

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
- [ ] Set `stvec` trap vector

### 2. Trap/Interrupt Subsystem

- [ ] Exception entry/exit (`sepc`/`scause`/`stval` dispatch)
- [ ] Timer interrupts (CLINT or SBI)
- [ ] External interrupts via PLIC
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
