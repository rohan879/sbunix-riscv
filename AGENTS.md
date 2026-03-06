# AGENTS.md - SBUnix Development Guide

This file provides guidance for agents working on the SBUnix RISC-V operating system project.

## Project Overview

SBUnix is a teaching OS kernel for RISC-V architecture (RV64IMAC). The project uses a cross-compiler toolchain (`riscv64-unknown-elf-gcc`) and runs on QEMU's `virt` machine.

## Build Commands

### Setting Up the Environment

```bash
cd sbunix-skeleton
source tools/sourceme.sh
```

This configures the PATH to include the RISC-V cross-compiler and QEMU.

### Building

```bash
make              # Build kernel and all user binaries
make clean        # Remove build artifacts
```

### Running

```bash
make qemu        # Build if needed and run kernel in QEMU (exit by pressing Ctrl-A then X)
qemu-riscv64 build/rootfs/bin/sbush  # Run shell in user-mode emulation
```

### Submitting

```bash
make submit      # Submit to /submit directory
```

## Testing

**There is no formal test framework.** Testing is done manually:

1. Build with `make` - must succeed with `-Wall -Werror`
2. Verify ELF binaries: `file build/rootfs/bin/sbush`
3. Disassemble to verify real code: `riscv64-unknown-elf-objdump -d build/rootfs/bin/sbush | head -40`
4. Verify packed in tarfs: `tar tf build/rootfs.tar | grep sbush`
5. Check symbols: `riscv64-unknown-elf-nm build/rootfs/bin/sbush | grep -E 'main|...'`
6. Run in QEMU: `make qemu`
7. Run user programs: `qemu-riscv64 build/rootfs/bin/sbush`

## Code Style Guidelines

### General Style

- **Language**: C
- **Indentation**: 2 spaces (no tabs)
- **Line endings**: Unix (LF)
- **Braces**: K&R style - function opening brace on new line:
  ```c
  void function(void) {
      // code
  }
  ```

### Naming Conventions

- **Functions**: snake_case (e.g., `allocproc`, `userinit`)
- **Variables**: snake_case (e.g., `current_proc`, `nextpid`)
- **Constants/Macros**: UPPER_SNAKE_CASE (e.g., `PAGE_SIZE`)
- **Structs/Enums**: lowercase (e.g., `struct proc`, `enum procstate`)
- **Type aliases**: suffix `_t` (e.g., `pagetable_t`)

### Header Files

- Use header guards:
  ```c
  #ifndef _PROC_H
  #define _PROC_H
  // ...
  #endif
  ```
- Include order: local headers first, then system headers
- Group related definitions (structs, enums, function declarations)

### Types

- Use fixed-width integers: `uint64_t`, `uint32_t`, `int64_t`, `int32_t`
- Use `size_t` for sizes, `ssize_t` for signed sizes
- Use `pid_t` for process IDs
- Prefer `unsigned` for bitfields and flags

### Functions

- Declare with return type on same line as name
- Parameters on same line if short, wrap if long
- Use `__attribute__((noreturn))` for functions that don't return
- Static functions for file-local helpers

### Error Handling

- Return `0` or positive for success, `-1` for error
- Check return values explicitly
- Release resources on error paths
- Set `errno` where appropriate in libc

### Memory Allocation

- Use `kalloc()` / `kfree()` in kernel
- Use `malloc()` / `free()` in user programs
- Always check allocation return values for NULL

### Inline Assembly

- Use `asm volatile()` for inline assembly
- Include memory barriers where necessary
- Comment hardware register accesses

## Project Structure

```
sbunix-skeleton/
├── kernel/          # OS kernel (C)
│   ├── *.c         # Kernel source
│   ├── *.S         # Assembly files
│   └── include/    # Kernel headers
├── libc/           # Standard C library
│   ├── *.c         # Library source
│   └── include/    # libc headers
├── bin/            # User programs
│   ├── sbush/     # Shell
│   ├── echo/      # echo utility
│   ├── cat/       # cat utility
│   └── ls/        # ls utility
├── tools/          # Build/development tools
└── rootfs/         # Root filesystem
```

## Commit Message Conventions

From `contrib.md`, use these prefixes:

| Prefix | Meaning |
|--------|---------|
| BF     | Bug fix |
| RF     | Refactoring |
| ENH    | Enhancement/new feature |
| OPT    | Optimization |
| BK     | Breaking changes |
| DOC    | Documentation |
| TEST   | Tests |
| STY    | Style/formatting |
| BLD    | Build changes |
| CHORE  | Miscellaneous |

Example: `BF: fix null pointer in allocproc`

## Important Notes

1. **Pre-existing issue**: Kernel syscall numbers (`kernel/include/syscall.h`) do not match libc's Linux RISC-V ABI numbers - this causes "Unknown syscall" errors (now fixed in recent work)
2. **No automated tests**: All testing is manual
3. **Development branches**: Work on separate branches, test locally before merging
4. **Cross-compilation**: All binaries must be built for RISC-V target

## Recent Session Work (CSE506 Spring 2026)

### Completed Features

#### 1. `nanosleep` Syscall (SYS_nanosleep = 101)
- Kernel handler (`kernel/trap.c:849-862`) sets `p->sleep_until = ticks + tv_sec` and `waiting_for_uart = 2`
- Timer interrupt wakes timed-sleep processes when `ticks >= sleep_until`
- `libc/unistd.c` provides `sleep()` wrapper using `timespec` struct

#### 2. Ctrl-C Support
- Kernel `handle_external_interrupt` kills all non-init processes when `0x03` is received
- Two code paths:
  - **Children killed** (`killed_any=1`): kernel prints `^C\n` for visual feedback, does NOT enqueue `0x03`
  - **Shell idle** (`!killed_any`): kernel enqueues `0x03` for `read_line` to handle
- `read_line` (`bin/sbush/sbush.c:14-52`) now prints `^C\n` when it receives `0x03`, then uses `goto restart:` to reprint the prompt cleanly
- Ctrl-C during sleep: wakes sleep and kills the child process; shell returns to REPL

#### 3. Backspace in Shell
- `read_line` handles `0x7f` and `0x08` with proper echo (`\x08 \x08` sequence)
- Guard `if (i > 0)` prevents erasing past the prompt

#### 4. `exit` Builtin
- Calls `SYS_exit` or `SYS_exit_group`
- When pid==1 (init) exits: kernel prints "System halted (init exited with status N)." and calls SBI shutdown
- Non-init processes become `ZOMBIE` and are reaped by parent

#### 5. Syscall ABI Alignment (kernel/include/syscall.h)
- Kernel `syscall.h` was missing most syscall numbers; added the full set matching the Linux RISC-V ABI:
  `SYS_getcwd(17)`, `SYS_chdir(49)`, `SYS_openat(56)`, `SYS_close(57)`, `SYS_pipe2(59)`,
  `SYS_getdents64(61)`, `SYS_dup3(24)`, `SYS_getpid(172)`, `SYS_clone(220)`, `SYS_execve(221)`,
  `SYS_nanosleep(101)`, plus `AT_FDCWD` and `SIGCHLD` constants
- Libc `syscall.h` and `unistd.h` updated to match: added `SYS_nanosleep = 101` and `sleep()` declaration
- Resolved pre-existing mismatch that caused "Unknown syscall" errors for most userspace operations

#### 6. `sleep` Binary (bin/sleep/sleep.c)
- New `sleep` binary replaces any prior stub; uses libc `sleep()` which calls `SYS_nanosleep`
- `simple_atou()` parses the seconds argument; prints usage error if no argument given

#### 7. `exec` Fix (kernel/exec.c)
- Track `sz` (process image size) correctly by updating it to `max(sz, segment_end)` for each PT_LOAD segment
- Switch `satp` to the new page table immediately after `exec()` builds it, before `trap_restore`'s `sret`, so the new address space is active on entry to user mode

#### 8. UART FIFO Constants (kernel/include/uart.h, kernel/uart.c)
- Added `UART_FCR` register address and FIFO control bit definitions (`FIFO_EN`, `RX_RESET`, `TX_RESET`, `TRIG_1`)
- Added comment to `uart_intr_init()` clarifying that it enables RX interrupts

### Dead Code Removed

1. **`kpipe.nread` field** (`kernel/trap.c:30`) — declared and initialized but never read; removed from struct and `alloc_pipe()`
2. **`yield()` function** (`kernel/proc.c:120-125`) — declared in `proc.h` but never called anywhere; removed with declaration
3. **Ghost declarations** (`kernel/include/vm.h:13,17`) — `uvmfirst()` and `uvminit()` declared but never implemented or called; removed

### Known Remaining Dead Code (Low Priority)

- **Display time functions** (`kernel/trap.c:104-137`): `print_two_digits()`, `display_key()`, `display_time()` run on every timer tick and UART interrupt but serve only cosmetic VT100 status bar (no functional impact)
- **Unreachable `while(1){}` guards** after `swtch()` and `trap_restore()` in `trap.c` — intentional panic guards; left in place
- **`SYS_getdents64` unimplemented** — `ls` utility calls this syscall (61) but kernel has no handler, making `ls_dir()` permanently unreachable
- **`set` builtin** (`bin/sbush/builtin.c`) — silently succeeds for wrong argument counts instead of printing error

#### 9. `mmap` / `munmap` / `mprotect` Syscalls
- **`SYS_mmap=222`, `SYS_munmap=215`, `SYS_mprotect=226`** added to `kernel/include/syscall.h`
- Anonymous `mmap` only (`MAP_ANONYMOUS=0x20`); file-backed mapping not required
- VA placement: `mmap_base` starts at `0x20000000` (512 MB), grows upward by rounded-up length
- `struct mmap_region { uint64_t va; uint64_t len; }` with `NMMAP=16` entries added to `struct proc` (`kernel/include/proc.h`)
- `allocproc()` zeroes `mmap_regions[]` and sets `mmap_base=0` (`kernel/proc.c`)
- `uvmunmap(pagetable_t, uint64_t va, uint64_t len)` added to `kernel/vm.c` + declared in `kernel/include/vm.h`
- Handlers implemented in `kernel/trap.c`; `mprotect` is a no-op returning 0

#### 10. `ls` Fix — Root Directory
- `ls` with no args calls `open(".", 0)`; `resolve_path` was producing `"/."` instead of `"/"`
- Fixed by `normalize_path()` which collapses `.` components (see below)

#### 11. `cd ..` / Path Normalization Fix
- `resolve_path` appended `..` literally, producing e.g. `/bin/..` which tarfs could not find
- Added `normalize_path()` to `kernel/trap.c`: processes absolute paths component-by-component, collapsing `.` (skip) and `..` (pop last segment)
- `resolve_path` now builds the raw combined path then calls `normalize_path` on it
- Verified in QEMU: `cd /bin` → `cd ..` → `ls` shows `etc bin` (root)

### QEMU Testing — Critical Notes

**`make qemu` requires a real interactive terminal** — it cannot be piped. Use `tmux`:

```bash
# Start a tmux session and boot QEMU inside it
tmux new-session -d -s sbunix_test -x 220 -y 50
tmux send-keys -t sbunix_test \
  "export PATH=/home/steven/College/Graduate_Spring_2026/CSE506/riscv-toolchain/bin:/home/steven/College/Graduate_Spring_2026/CSE506/qemu/bin:$PATH" Enter
sleep 0.5
tmux send-keys -t sbunix_test \
  "qemu-system-riscv64 -machine virt -bios default -kernel /path/to/build/kernel.elf -nographic 2>/tmp/qemu_err.txt" Enter

# Wait for boot (~8s), then capture screen
sleep 8
tmux capture-pane -t sbunix_test -p | tail -5
# Should show: "sbush$"

# Send commands and capture output
tmux send-keys -t sbunix_test "ls" Enter
sleep 2
tmux capture-pane -t sbunix_test -p | tail -10

# Exit cleanly
tmux send-keys -t sbunix_test "exit" Enter
sleep 2
tmux kill-session -t sbunix_test
```

**Key QEMU flags**:
- Must use `-bios default` (not `-bios none`) — without SBI/OpenSBI the kernel produces no output
- `-nographic` maps serial to stdio
- QEMU binary: `/home/steven/College/Graduate_Spring_2026/CSE506/qemu/bin/qemu-system-riscv64`
- Kernel ELF: `build/kernel.elf` (entry point `0x80200000`)
- Do NOT pipe stdin/stdout to QEMU without a real tty; use tmux

### Testing Notes

All regression tests pass (128/128):
```
echo hello                          # Simple command
echo hello | cat                    # Single pipe
echo hello world | cat | cat        # Multi-pipe chains
sleep 2                             # Waits ~2 seconds (scheduler-based, not busy-wait)
sleep 10 (then Ctrl-C)             # Kills sleep, returns to prompt with "^C\n"
Ctrl-C at idle prompt              # Prints "^C\n", reprints prompt on new line
ls                                  # Lists: etc bin
ls /bin                             # Lists all 6 binaries
cd /bin                             # Changes to /bin directory
cd ..                               # Returns to / (normalize_path handles ..)
test                                # Runs test suite: 128 passed, 0 failed
exit                                # Cleanly halts the system
```

### Important Code Patterns

**Kernel read path for stdin** (`trap.c:506-533`):
- Returns one raw byte per `SYS_read` call
- `\r` is converted to `\n` by kernel
- `0x03` (Ctrl-C) may arrive either via interrupt (prints `^C\n` in kernel if children exist) or via direct poll (no kernel print; `read_line` handles it)

**Process wait logic** (`trap.c:292-323`):
- `sys_wait4` sleeps with `waiting_for_uart = 0` (not a UART-input sleep)
- External interrupt wakes all `SLEEPING` processes, allowing `waitpid` to return when children exit

**Pipe design** (`trap.c:22-54`):
- Fixed-size circular buffer (PIPE_BUF = 4096 bytes)
- Writers block if full; readers block if empty
- Refcounts track open read/write ends; pipe deallocated when both reach 0
