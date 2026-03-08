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

### 1. Boot + Early Console — DONE

- [x] OpenSBI handoff, UART printing
- [x] Parse DTB pointer from `a1`
- [x] Set `stvec` trap vector

### 2. Trap/Interrupt Subsystem

- [x] Exception entry/exit (`sepc`/`scause`/`stval` dispatch)
- [x] Timer interrupts (SBI)
- [x] External interrupts via PLIC
- [x] Minimal scheduler tick for preemption — `handle_timer_interrupt()` yields RUNNING process via `swtch()` for round-robin

### 3. Memory Subsystem

- [x] Physical page allocator (page descriptors, free list)
- [x] Sv39 page tables (3-level, managed via `satp` CSR)
- [x] Enable/disable virtual memory
- [x] `kmalloc` — power-of-two slab allocator in `kernel/kmalloc.c` (16–2048 byte buckets)

### 4. Process Subsystem

- [x] Kernel threads
- [x] Context switch (RISC-V register save/restore)
- [x] User-mode entry (U-mode), per-process page tables

### 5. User-level Subsystem

- [x] VMAs / vm_map_entry tracking — `struct vm_area` linked list in `kernel/vm.c`, integrated with exec/mmap/munmap/fork/exit
- [x] COW fork (via `clone`) — PTE_COW (RSW bit 8), page refcounting in kalloc
- [x] Auto-growing stack — page fault handler extends stack down by up to 16 pages
- [x] Page fault handler (`scause` 13/15) — COW faults, stack auto-grow, mmap demand paging

### 6. I/O Subsystem

- [x] Syscall dispatch (`ecall` → `scause` environment-call-from-U-mode)
- [x] `read(stdin)`, `write(stdout)`, `write(stderr)` via UART terminal
- [x] VFS layer — `kernel/vfs.c` with mount table, `struct vfs_ops`, `struct open_file` refcounting; tarfs adapter (`kernel/tarfs_vfs.c`), devfs (`kernel/devfs.c`)
- [x] tarfs: `open`, `read`, `close`, `opendir`, `readdir`, `closedir`
  - Embedded in `.tarfs` section (`_tarfs_start` / `_tarfs_end`)

### 7. Userspace Binaries

- [x] `bin/echo`, `bin/sleep`, `bin/cat`, `bin/ls`
- [x] `bin/kill` (`kill -9`), `bin/ps`
- [x] `bin/sbush` (full shell)
- [x] `bin/spin` — preemptive scheduling test (two CPU-bound fork'd loops)
- [x] `bin/alarmtest` — SIGALRM test (`alarm(2)` + sigaction handler)
- [x] `bin/segvtest` — SIGSEGV test (NULL deref caught by user signal handler)

### 8. /bin/init

- [x] fork(clone) + exec + wait on `/etc/rc` — `bin/init/init.c` forks+execs `/bin/sbush /etc/rc`
- [x] Then exec sbush — `init` respawns interactive shell after rc completes; `userinit()` now boots `/bin/init`

## POSIX Shell Completeness (sbush)

Shell basics (cd, PATH, pipes, background, scripts) are done. These remain for POSIX compliance:

### Signal Handling

- [x] Install `SIGINT` handler — forwards to `fg_pid` via `sigaction(SA_RESTART)` in `sbush.c`
- [x] Install `SIGTSTP` (^Z) handler — forwards to `fg_pid`, shell itself never stops
- [x] Handle `SIGCHLD` — `sigchld_handler` reaps via `waitpid(WNOHANG|WUNTRACED)`, updates job table

### Job Control

- [x] `jobs` builtin — list background/stopped jobs (`bin/sbush/jobs.c`)
- [x] `fg` builtin — resume stopped job in foreground
- [x] `bg` builtin — resume stopped job in background
- [x] Set process groups for pipelines (`setpgid`) and terminal control (`tcsetpgrp`) — first child calls `setpgid(0,0)`, others join; parent uses `tcsetpgrp` for foreground

### Redirections

- [x] Output redirect: `cmd > file`, `cmd >> file`
- [x] Input redirect: `cmd < file`
- [x] Stderr redirect: `cmd 2> file`, `cmd 2>&1`

### Variable / Expansion

- [x] `$?` — exit status of last command (deferred expansion per-pipeline)
- [x] `$!` — PID of last background process
- [x] `$$` — PID of shell itself
- [x] Double-quote variable expansion (`"$VAR"` preserves spaces)
- [x] Single-quote literal strings (`'no $expansion'`)
- [x] Backslash escapes (`\"`, `\\`, `\n`, `\t`, `\$`)

### Globbing

- [x] `*` wildcard expansion (match any string) — implemented in `parse.c:72-209`
- [x] `?` wildcard expansion (match single character) — implemented in `parse.c:72-209`

## Libc Header Completeness (Feedback-Driven) — DONE

Originally scored 13/49 tests (20/100 points). All missing headers, declarations, and implementations have been added.

### `libc/include/sys/` Directory — Complete

All 10 sys/ headers exist with full content.

- [x] `mkdir -p libc/include/sys` — already exists
- [x] `sys/time.h` — `struct timeval`, `gettimeofday()`, `timersub`/`timeradd`/`timercmp` macros
- [x] `sys/syscall.h` — re-exports all `SYS_*`, adds `__NR_*` POSIX aliases
- [x] `sys/types.h` — `pid_t`, `uid_t`, `gid_t`, `off_t`, `ssize_t`, `size_t`, `mode_t`
- [x] `sys/stat.h` — `struct stat`, `S_ISREG`/`S_ISDIR` macros, `fstat()`/`stat()` declarations
- [x] `sys/wait.h` — `waitpid()`, `WIFEXITED`/`WEXITSTATUS`/`WIFSIGNALED`/`WTERMSIG` macros
- [x] `sys/mman.h` — `mmap()`/`munmap()`/`madvise()` declarations, `PROT_*`/`MAP_*`/`MADV_*` constants
- [x] `sys/ioctl.h` — `ioctl()`, `struct winsize`, `TIOCGWINSZ`/`TIOCSWINSZ`/`TIOCGPGRP`/`TIOCSPGRP`
- [x] `sys/resource.h` — `struct rlimit`, `struct rusage`, `RLIMIT_*` constants, `getrlimit()`/`setrlimit()`/`getrusage()`
- [x] `sys/utsname.h` — `struct utsname`, `uname()`
- [x] `sys/uio.h` — `struct iovec`, `readv()`/`writev()`

### Additional Headers — Complete

- [x] `ctype.h` — 14 `static inline` character classification functions (`isalpha`, `isdigit`, `toupper`, etc.)
- [x] `assert.h` — `assert()` macro using `dprintf(2, ...)` + `abort()`
- [x] `termios.h` — `struct termios` (matches kernel `tty.h`), `tcgetattr()`/`tcsetattr()`/`cfmakeraw()`, all flag constants
- [x] `libgen.h` — `basename()`/`dirname()`
- [x] `poll.h` — `struct pollfd`, `nfds_t`, `POLLIN`/`POLLOUT`/`POLLERR`/`POLLHUP`/`POLLNVAL`, `poll()`
- [x] `setjmp.h` — `jmp_buf` (14 callee-saved regs), `setjmp()`/`longjmp()`
- [x] `pwd.h` — `struct passwd`, `getpwuid()`/`getpwnam()`
- [x] `grp.h` — `struct group`, `getgrgid()`/`getgrnam()`

### Missing Declarations in Existing Headers

- [x] `string.h` — added: `memmove`, `memchr`, `strdup`, `strndup`, `strncat`, `strspn`, `strcspn`, `strpbrk`, `strtok`, `strerror`
- [x] `unistd.h` — added: `isatty`, `setsid`, `getpgrp`, `setpgid`, `tcgetpgrp`, `tcsetpgrp`, `alarm`, `getpgid`, `getsid`, `ftruncate`, `geteuid`, `getegid`, `wait`
- [x] `signal.h` — added: `sighandler_t` typedef, `sig_atomic_t` typedef
- [x] `stdlib.h` — added: `setenv`, `unsetenv`, `putenv`
- [x] `errno.h` — complete (45 error codes, extern errno)
- [x] `fcntl.h` — complete (`O_*` flags, `AT_FDCWD`, fcntl commands)
- [x] `dirent.h` — complete (`struct dirent`, `DIR`, opendir/readdir/closedir)
- [x] `stdio.h` — added: `FILE` typedef, `stdin`/`stdout`/`stderr`, `fprintf`/`vfprintf`/`perror`/`fputs`/`fputc`/`fgets`/`fgetc`/`fwrite`/`fread`/`fopen`/`fclose`/`fflush`/`fileno`/`feof`/`ferror`/`clearerr`/`fseek`/`ftell`/`sscanf`
- [x] `syscall.h` — added: `SYS_ftruncate`, `SYS_tgkill`, `SYS_rt_sigreturn`, `SYS_madvise`

### Corresponding Libc Implementations

- [x] Implement missing string functions in `libc/string.c` — all 10 functions
- [x] Implement `isatty()` in `libc/unistd.c` — ioctl probe + fd 0-2 fallback
- [x] Implement `setenv()`/`unsetenv()`/`putenv()` in `libc/stdlib.c` — static env storage
- [x] `stat()`/`fstat()` syscall wrappers in libc
- [x] `waitpid()` wrapper with `W*` macros
- [x] `ioctl` syscall handling in `kernel/trap.c` — SYS_ioctl(29), TIOCGWINSZ returns 80x24
- [x] FILE* streams in `libc/fileio.c` — static pool of 16 FILEs, unbuffered fprintf/fread/fwrite/fopen/fclose/perror
- [x] Terminal I/O in `libc/termios.c` — tcgetattr/tcsetattr wrapping ioctl TCGETS/TCSETS, cfmakeraw
- [x] Resource limits in `libc/resource.c` — getrlimit/setrlimit via prlimit64, getrusage stub
- [x] System identification in `libc/utsname.c` — uname() returns "SBUnix"/"riscv64"
- [x] Scatter/gather I/O in `libc/uio.c` — readv/writev syscall wrappers
- [x] Path decomposition in `libc/libgen.c` — basename/dirname pure string manipulation
- [x] I/O multiplexing in `libc/poll.c` — poll() stub (assumes all events ready)
- [x] Non-local jumps in `libc/setjmp.S` — RISC-V setjmp/longjmp saving 14 callee-saved registers
- [x] User/group database in `libc/pwd.c` — getpwuid/getpwnam/getgrgid/getgrnam returning static root entries
- [x] Fixed libc wrappers to use real syscalls — setsid, getpgrp, setpgid, tcgetpgrp, tcsetpgrp now call kernel
- [x] Added missing wrappers — getpgid, getsid, ftruncate, geteuid, getegid, wait, madvise
- [x] Fixed `abort()` to raise SIGABRT before _exit(134)

## Kernel Syscall Stubs (Feedback-Driven)

Syscall handlers added to `kernel/trap.c` to satisfy POSIX test expectations:

- [x] `SYS_ioctl` (29) — TIOCGWINSZ returns 80x24, TIOCGPGRP/TIOCSPGRP handled, unknown → -ENOTTY
- [x] `SYS_rt_sigaction` (134) — stores/retrieves sigaction per process, rejects SIGKILL/SIGSTOP
- [x] `SYS_rt_sigprocmask` (135) — SIG_BLOCK/SIG_UNBLOCK/SIG_SETMASK on `proc->sigmask`
- [x] `SYS_gettimeofday` (169) — Goldfish RTC epoch time via get_wall_time()
- [x] `SYS_clock_gettime` (113) — epoch time + VA translation via walk()
- [x] `SYS_fcntl` (25) — F_GETFD/F_SETFD/F_GETFL/F_SETFL stubs
- [x] `SYS_faccessat` (48) — resolves path in tarfs, returns 0 or -ENOENT
- [x] `SYS_unlinkat` (35), `SYS_linkat` (37), `SYS_renameat2` (276), `SYS_mkdirat` (34) — return -EROFS
- [x] `SYS_umask` (166) — returns 022
- [x] `execve` envp fix — copies envp strings before destroying old page table
- [x] `SYS_alarm` (200) — custom syscall; sets `proc->alarm_ticks`, returns remaining seconds from previous alarm
- [x] `SYS_geteuid` (175), `SYS_getegid` (177) — return 0 (root)
- [x] `SYS_set_tid_address` (96) — returns current pid
- [x] `SYS_readlinkat` (78) — returns -ENOENT (no symlinks)
- [x] `SYS_getrandom` (278) — LCG PRNG seeded from r_time()
- [x] `SYS_writev` (66), `SYS_readv` (65) — basic iovec iteration (partial, see Remaining Fixes)
- [x] `SYS_prlimit64` (261) — returns -ENOSYS stub
- [x] `SYS_setpgid` (154), `SYS_getpgid` (155), `SYS_setsid` (157), `SYS_getsid` (156) — pid-returning stubs
- [x] `SYS_madvise` (233) — no-op success (no real VM advice needed)
- [x] `SYS_ftruncate` (46) — returns -EROFS (tarfs is read-only)
- [x] `SYS_tgkill` (131) — routes to same signal delivery logic as SYS_kill

## Kernel Architecture Features

### Signal Delivery

- [x] Signal trampoline page at `SIG_TRAMPOLINE_VA` (0x3FFFFFF000) — `kernel/sigtramp.S`
- [x] `deliver_signal()` — saves trapframe, diverts to handler, sets ra to trampoline
- [x] `SYS_rt_sigreturn` (139) — restores saved trapframe
- [x] `SYS_kill` uses `sigpending` bits for non-SIGKILL signals
- [x] Per-process `sigactions[32]`, `sigmask`, `sigpending` in `struct proc`
- [x] SIGCHLD delivery on child exit — `SYS_exit`, `SYS_kill` SIGKILL, and `deliver_signal` SIG_DFL paths all set `parent->sigpending |= (1 << SIGCHLD)`
- [x] SIGSEGV delivery via signal machinery — page fault handler sets `sigpending |= (1 << SIGSEGV)` and calls `deliver_signal()` instead of direct kill; user handlers can catch it
- [x] `SYS_alarm` (200) — sets per-process `alarm_ticks`; timer handler checks expiry and delivers SIGALRM (signal 14)
- [x] `deliver_signal()` SIG_DFL cleanup — calls `vma_free_all()` + `close_ofile()` for all fds before marking ZOMBIE (mirrors `SYS_exit` path)
- [x] Ctrl-C delivers SIGINT via `sigpending` instead of force-killing processes — lets sigaction handlers run properly

### Sub-page Allocator

- [x] `kmalloc(size)` / `kmfree(ptr, size)` — `kernel/kmalloc.c`
- [x] 8 power-of-two bucket sizes: 16, 32, 64, 128, 256, 512, 1024, 2048 bytes
- [x] Each bucket backed by freelist carved from kalloc'd pages

### COW Fork + Page Refcounting

- [x] `copy_pagetable_cow()` — marks writable user pages read-only + PTE_COW in both parent/child
- [x] Page refcount array in `kalloc.c` — `kalloc()` sets refcount=1, `kfree()` frees when refcount=0
- [x] Page fault handler: COW fault allocates new page + copies if refcount > 1

### Stack Auto-Grow + Demand Paging

- [x] Page fault extends stack down by up to 16 pages when fault is near stack bottom
- [x] Demand paging for mmap regions — lazily maps pages on first access

### Remaining Kernel Work

- [x] Shell-side signal handlers (SIGINT, SIGTSTP, SIGCHLD) — installed in `sbush.c` via `sigaction`
- [x] Process groups for pipelines — `setpgid`/`tcsetpgrp` used in `exec.c` for pipeline job control
- [x] VFS layer — `kernel/vfs.c` + `tarfs_vfs.c` + `devfs.c` with mount table and `open_file` refcounting
- [x] `/bin/init` runs `/etc/rc` before exec'ing sbush — `bin/init/init.c`

### Still Open

- [x] Kernel SIGCHLD generation — `SYS_exit`, SIGKILL, and `deliver_signal` SIG_DFL all set `parent->sigpending |= (1 << SIGCHLD)` on child death
- [x] devfs integration for stdin/stdout/stderr — fd 0/1/2 opened via `/dev/console` VFS ops in `init_starter()`; UART special-case removed from trap.c
- [x] Input discipline (termios) — `kernel/tty.c` line discipline; cooked mode with echo/backspace/^C/^D; raw mode; ioctl TCGETS/TCSETS
- [x] User-level threads — `sys_clone` extended with CLONE_VM/CLONE_FILES/CLONE_SIGHAND; refcounted shared resources
- [x] Super pages — Sv39 2MB megapages for kernel identity maps; `walk()` detects level-1 leaf PTEs; COW fork splits to 4KB
- [x] On-disk read/write file system — ext2-lite over virtio-blk (`kernel/ext2.c`); mounted at `/disk`; open/read/write/mkdir/unlink
- [x] On-disk swap and paging — clock eviction in `kernel/swap.c`; 1024 swap slots at end of disk; swap-in on page fault
- [ ] TCP/IP networking via lwIP (see detailed plan below)
- [ ] Multi-core support

### Remaining Fixes

- [x] writev/readv VA translation — iovec array and buffer pointers translated via walk()
- [x] getrandom — LCG PRNG seeded from RTC; functional but not cryptographically secure
- [x] prlimit64 — RLIMIT_STACK (8MB) and RLIMIT_NOFILE (16) handled; others return RLIM_INFINITY
- [x] Process groups — setpgid/getpgid/setsid/getsid with pgid/sid fields in struct proc
- [x] Libc errno handling — `__syscall_ret` applied to all error-returning syscall wrappers in unistd.c and stat.c
- [x] nanosleep VA translation — user timespec pointer translated via walk()
- [x] Ext2 timestamps — ext2_stat/ext2_fstat populate st_atime/st_mtime/st_ctime from on-disk inode

## POSIX Compliance Checklist (Required Components)

- [x] Memory Manager — physical page allocator, Sv39 page tables, kmalloc slab allocator, COW fork, demand paging
- [x] Creation of Virtual Address Space — per-process page tables, VMAs (`struct vm_area`), mmap/munmap/mprotect
- [x] Kernel — boot via OpenSBI, trap/interrupt dispatch, SBI timer, PLIC external interrupts
- [x] Scheduler — cooperative + preemptive round-robin; timer-driven yield via `swtch()`
- [x] Sleeping Process Support — SLEEPING state in `struct proc`; nanosleep, waitpid, tty_read all sleep/wake
- [x] WFI/Idle Support — `wfi` instruction in scheduler loop when no RUNNABLE process found (`kernel/proc.c`)
- [x] Threads — `sys_clone` with `CLONE_VM`/`CLONE_FILES`/`CLONE_SIGHAND`; refcounted shared resources
- [x] File System — VFS layer (`kernel/vfs.c`) with mount table (8 slots), `struct vfs_ops`, `struct open_file` refcounting
- [x] FS Type 1: Read-Only FS at boot — tarfs mounted at `/` with ls, ps, cat, echo, kill, sleep, sbush
- [x] FS Type 2: /proc virtual FS — `kernel/procfs.c` exposes per-PID directories with `status` and `cmdline`; mounted at `/proc` in `vfs_init()`
- [x] Mount points — VFS mount table supports 8 entries; currently: `/` (tarfs), `/dev` (devfs), `/disk` (ext2)
- [x] R/W file system — ext2-lite over virtio-blk at `/disk`; open/read/write/mkdir/unlink
- [x] Terminal Support — `kernel/tty.c` line discipline; cooked mode (echo, backspace, ^C, ^D); raw mode; stdin/stdout/stderr via `/dev/console`
- [x] Job Control Shell — sbush with sessions (`setsid`), process groups (`setpgid`/`tcsetpgrp`), fg/bg/jobs builtins
- [x] Pipes — bidirectional pipe2 syscall; shell pipelines work across processes
- [x] Limit System — `prlimit64` handles RLIMIT_STACK (8MB) and RLIMIT_NOFILE (16); libc getrlimit/setrlimit wrappers
- [ ] Networks — no TCP/IP stack; lwIP integration not started (see detailed plan below)
- [x] getrandom — LCG PRNG seeded from RTC via `r_time()`; functional but not cryptographically secure
  - **Note:** Implemented in commit `d72182c` (`kernel/trap.c`, SYS_getrandom handler). Uses a simple linear congruential generator — sufficient for test harnesses and non-security uses, but does not meet POSIX `getentropy()`/`getrandom()` entropy requirements. A hardware RNG or entropy pool would be needed for real crypto.

### Remaining Work for Full POSIX Compliance

- [x] Add WFI to scheduler idle loop — `wfi` halts hart until next interrupt when no RUNNABLE process exists
- [x] Implement procfs (`/proc`) — `kernel/procfs.c` with `status`, `cmdline` per-PID; mounted at `/proc`
- [ ] TCP/IP networking via lwIP (3 layers, detailed below)

### TCP/IP Networking Plan (lwIP + virtio-net)

QEMU already provides `virtio-net-pci-non-transitional` with user-mode networking (`-netdev user`). The device is present on the PCI bus but no kernel driver initializes it.

**Existing infrastructure that lwIP can reuse:**
- PCI ECAM enumeration — working in `kernel/virtio_blk.c`; virtio-net uses the same pattern (device ID `0x1041` vs `0x1042`)
- `kmalloc()`/`kalloc()` — lwIP needs malloc/free for packet buffers and control blocks
- PLIC interrupt routing — already working for UART; virtio-net RX interrupt follows same path
- Kernel `string.h` — memcpy, memset, strlen already implemented (lwIP requires these)
- Timer tick counter — `ticks` global incremented by timer interrupt; lwIP needs millisecond time

**Layer 1: virtio-net driver** (`kernel/virtio_net.c`)
- [ ] Clone virtio-blk PCI discovery; scan for device ID `0x1041` (virtio-net non-transitional)
- [ ] Set up 2 virtqueues: RX (queue 0) and TX (queue 1), vs 1 for virtio-blk
- [ ] Negotiate features: `VIRTIO_NET_F_MAC` (bit 5) to read MAC address from device config
- [ ] Pre-post RX buffers (descriptor chain with virtio-net header + packet data)
- [ ] TX path: format virtio-net header + ethernet frame, submit to TX virtqueue
- [ ] RX interrupt handler: claim from PLIC, dequeue completed RX buffers, feed to lwIP `netif->input()`
- [ ] Expose `virtio_net_send(buf, len)` and `virtio_net_init()` to kernel

**Layer 2: lwIP platform port** (`kernel/lwip/`)
- [ ] `lwipopts.h` — configure for NO_SYS=1 (no threading), MEM_SIZE, PBUF pool sizes, enable TCP/UDP/DHCP
- [ ] `cc.h` / `arch.h` — type mappings (`u8_t`=`uint8_t`, etc.), `PACK_STRUCT_*` macros, `LWIP_PLATFORM_DIAG` → `printk`
- [ ] `sys_arch.c` — `sys_now()` returns milliseconds from tick counter; no mutex/semaphore needed (NO_SYS=1)
- [ ] Memory hooks: `mem_malloc`/`mem_free` → wrap `kmalloc`/`kmfree`; or use lwIP's internal allocator with a static pool from `kalloc()`
- [ ] `netif` driver: register with `netif_add()`, set `linkoutput` → `virtio_net_send()`, call `ethernet_input()` from RX interrupt
- [ ] Call `sys_check_timeouts()` from timer tick to drive lwIP timers (ARP, TCP retransmit, DHCP)
- [ ] Build lwIP sources with cross-compiler flags (`-ffreestanding -nostdinc -mcmodel=medany`)

**Layer 3: Socket syscalls** (kernel/trap.c + libc)
- [ ] Kernel side: add syscall handlers for `SYS_socket` (198), `SYS_bind` (200), `SYS_listen` (201), `SYS_accept` (202), `SYS_connect` (203), `SYS_sendto` (206), `SYS_recvfrom` (207), `SYS_setsockopt` (208), `SYS_getsockopt` (209)
- [ ] Dispatch to lwIP's sequential API: `lwip_socket()`, `lwip_bind()`, `lwip_connect()`, `lwip_send()`, `lwip_recv()`
- [ ] Map lwIP socket fds into the per-process open file table (new `struct open_file` type)
- [ ] Libc headers: `sys/socket.h`, `netinet/in.h`, `arpa/inet.h` with socket wrappers

**Validation:**
- [ ] Milestone 1: `virtio_net_init()` prints MAC address at boot — confirms PCI discovery works
- [ ] Milestone 2: DHCP acquires IP from QEMU's built-in DHCP server (10.0.2.x)
- [ ] Milestone 3: `ping 10.0.2.2` (QEMU gateway) from kernel — confirms ICMP round-trip
- [ ] Milestone 4: Userspace UDP echo client/server
- [ ] Milestone 5: Userspace TCP connect to host via QEMU port forwarding

## Grading Targets

- [x] Cooperative multi-tasking + all functional requirements (50 pts) — scheduler, fork, exec, wait, pipes, VFS, tarfs, user binaries all working
- [x] Preemptive multi-tasking (+10 pts) — timer-driven scheduler tick yields RUNNING processes via `swtch()`; tested with `bin/spin` (two CPU-bound fork'd loops both complete)
- [x] Signals (INT on ^C, ALARM, SEGV) and pipes (+10 pts) — SIGINT via `sigpending`, SIGALRM via `alarm()` syscall, SIGSEGV via `deliver_signal()`, SIGCHLD on exit; pipes work; tested with `spin`, `alarmtest`, `segvtest`
- [x] On-disk r/w file system (+10 pts) — ext2-lite with block cache, inode cache, directory ops over virtio-blk
- [x] On-disk swap and paging (+10 pts) — clock eviction, swap bitmap, PTE_SWAP encoding, transparent swap-in on page fault
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
