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

## Warm-up Project 3 (Part 1): Device Discovery — DONE

- [x] Parse DTB (passed in `a1` at boot) to enumerate devices
  - UART, PLIC, CLINT, virtio-mmio regions, PCIe host bridge
  - RISC-V equivalent of x86 PCI config space walk
- [x] Locate the virtio-blk block device

## Warm-up Project 3 (Part 2): Block Device (virtio-blk) — DONE

- [x] virtio-blk driver over virtio-mmio (replaces AHCI on x86)
- [x] Write 100 x 4KB blocks (block N filled with byte N)
- [x] Read back and verify correctness

## Final Project — Plan of Attack

### 1. Boot + Early Console (partially done)

- [x] OpenSBI handoff, UART printing
- [x] Parse DTB pointer from `a1`
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

## Automated Tests

Testing architecture: kernel-side tests run from `boot()` before userspace; userspace tests run as shell scripts via `/etc/rc`. All tests print `[PASS] name` or `[FAIL] name` to serial. A host-side script (`tools/run_tests.sh`) launches QEMU with a timeout, captures serial output, and counts results.

### Test Infrastructure

- [ ] `kernel/test.c`: kernel test framework with `TEST(name, expr)` macro
- [ ] `rootfs/etc/test.sh`: userspace test suite (run from `/etc/rc`)
- [ ] `tools/run_tests.sh`: host-side runner (timeout + parse serial output)
- [ ] `make test` target in Makefile

### 2. Trap/Interrupt Subsystem Tests

- [ ] `trap_vector_set` (kernel): `stvec` CSR is non-zero after setup
- [ ] `timer_fires` (kernel): timer callback increments counter; assert counter > 0 after delay loop
- [ ] `timer_monotonic` (kernel): two successive timer reads yield t2 > t1
- [ ] `plic_claim_complete` (kernel): PLIC claim returns valid IRQ for UART; complete doesn't fault
- [ ] `preempt_tick` (kernel): two kernel threads both get scheduled within N timer ticks

### 3. Memory Subsystem Tests

- [ ] `palloc_single` (kernel): `palloc()` returns non-NULL, page-aligned address
- [ ] `palloc_free_reuse` (kernel): free a page, allocate again — get same page back
- [ ] `palloc_exhaust` (kernel): allocate until NULL; count matches expected free pages
- [ ] `pt_map_read` (kernel): map a page at a VA, write through PA, read through VA — values match
- [ ] `pt_unmap` (kernel): unmap a page, access causes page fault (trap handler sets a flag)
- [ ] `pt_kernel_identity` (kernel): after enabling `satp`, kernel text/data still accessible
- [ ] `kmalloc_basic` (kernel): `kmalloc(64)` returns non-NULL; write/read back works
- [ ] `kmalloc_alignment` (kernel): returned pointer is at least 8-byte aligned
- [ ] `kmalloc_multiple` (kernel): multiple allocations return non-overlapping regions

### 4. Process Subsystem Tests

- [ ] `kthread_create_run` (kernel): create kernel thread, it sets a global flag; main thread sees flag set
- [ ] `kthread_two_threads` (kernel): two threads each increment separate counters; both > 0 after yield loop
- [ ] `context_switch_regs` (kernel): thread stores known values in saved regs, switches out and back — regs preserved
- [ ] `user_mode_entry` (kernel): jump to U-mode code that does `ecall`; trap handler sees `scause == 8`
- [ ] `per_process_pt` (kernel): two processes map same VA to different PAs; each reads its own value

### 5. User-level Subsystem Tests (VMAs, COW, Page Faults)

- [ ] `vma_track` (kernel): after mapping a region, VMA list contains entry with correct start/end/prot
- [ ] `cow_fork_isolation` (kernel/user): parent writes X, fork, child writes Y — parent still sees X
- [ ] `cow_fork_page_count` (kernel): after fork, physical page count doesn't double (pages shared)
- [ ] `stack_autoexpand` (user): recursive function touches stack well below initial SP — no crash
- [ ] `segv_on_bad_addr` (user): dereference `0xDEADBEEF` → process killed (parent sees non-zero exit via waitpid)
- [ ] `page_fault_demand` (kernel): map VMA without backing page; first access triggers fault, handler allocates, second access succeeds

### 6. I/O Subsystem Tests

- [ ] `syscall_write_stdout` (user): `write(1, "hello", 5)` → "hello" appears in serial output
- [ ] `syscall_read_stdin` (user): pipe "abc" to child, `read(0, buf, 3)` returns 3, buf == "abc"
- [ ] `syscall_open_close` (user): `open("/etc/rc")` returns fd >= 0; `close(fd)` returns 0
- [ ] `syscall_bad_fd` (user): `read(999, buf, 1)` returns -EBADF
- [ ] `tarfs_open_read` (user): `open("/etc/rc")` + `read()` returns file contents matching expected
- [ ] `tarfs_opendir` (user): `opendir("/bin")` succeeds; `readdir` yields "cat", "ls", "echo", "sbush"
- [ ] `tarfs_file_not_found` (user): `open("/nonexistent")` returns -ENOENT
- [ ] `tarfs_read_binary` (user): read first 4 bytes of `/bin/echo` → matches ELF magic `\x7fELF`
- [ ] `pipe_basic` (user): `pipe()`, write "xyz" to write-end, read from read-end → "xyz"
- [ ] `pipe_close_read_end` (user): close read end, write → SIGPIPE or -EPIPE

### 7. Userspace Binary Tests

- [ ] `echo_basic` (user-script): `echo hello world` → stdout is "hello world\n"
- [ ] `echo_no_args` (user-script): `echo` → stdout is "\n"
- [ ] `cat_file` (user-script): `cat /etc/rc` → output matches known rc contents
- [ ] `cat_nonexistent` (user-script): `cat /nope` → non-zero exit
- [ ] `ls_bin` (user-script): `ls /bin` → output contains "echo", "cat", "ls", "sbush"
- [ ] `ls_nonexistent` (user-script): `ls /nope` → non-zero exit
- [ ] `sleep_duration` (user-script): `sleep 1` completes without crash (verify exit 0)
- [ ] `kill_process` (user-script): launch background `sleep 999`, then `kill -9 $!` → waitpid returns
- [ ] `ps_output` (user-script): `ps` output includes at least init and current shell

### 8. Shell (sbush) Tests

- [ ] `sh_simple_cmd` (user-script): `/bin/echo hello` → "hello"
- [ ] `sh_builtin_cd` (user-script): `cd /bin && /bin/ls` → shows binary names
- [ ] `sh_pipe` (user-script): `echo hello | cat` → "hello"
- [ ] `sh_multi_pipe` (user-script): `echo abc | cat | cat` → "abc"
- [ ] `sh_env_var` (user-script): `export FOO=bar && echo $FOO` → "bar"
- [ ] `sh_path_resolve` (user-script): `export PATH=/bin && echo works` → "works"
- [ ] `sh_background` (user-script): `sleep 0 &` doesn't block shell; next command runs
- [ ] `sh_script` (user-script): write and run a script, check output
- [ ] `sh_exit_code` (user-script): `false; echo $?` → non-zero (once `$?` supported)

### 9. /bin/init Tests

- [ ] `init_runs_rc` (user-script): boot output includes "Running /etc/rc"
- [ ] `init_spawns_shell` (user-script): after rc, shell prompt appears (PS1 printed)
- [ ] `init_reaps_zombies` (user-script): fork a child that exits; after wait, `ps` shows no zombie

### Grading Target Tests (Bonus)

- [ ] `preempt_fairness` (kernel): two CPU-bound loops each get roughly equal time slices
- [ ] `signal_sigint` (user): `^C` kills foreground process; shell survives
- [ ] `signal_segv` (user): null deref in child → child killed with SIGSEGV, parent gets status
- [ ] `signal_alarm` (user): `alarm(1)` → SIGALRM delivered after ~1 second
- [ ] `disk_write_read` (kernel): write pattern to disk blocks, read back, verify (existing WP3 test)
- [ ] `swap_pressure` (kernel): allocate more pages than physical RAM; pages evicted to disk and faulted back
- [ ] `pipe_across_procs` (user): `echo test | cat` across two separate processes via shell pipe

### Test Priority Order

Build tests in this order (matching implementation order):

1. Memory subsystem tests — validates the foundation everything else depends on
2. Process/context-switch tests — needed before any userspace works
3. Syscall + I/O tests — validates the kernel-user boundary
4. Tarfs tests — needed before any binary can be loaded
5. Binary tests (echo, cat, ls) — quick smoke tests
6. Shell tests — integration-level, depend on everything above
7. Bonus tests (signals, swap, etc.) — add as each feature is implemented
