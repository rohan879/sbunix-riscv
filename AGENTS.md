# AGENTS.md - SBUnix Development Guide

This file provides guidance for agents working on the SBUnix RISC-V operating system project.

## Project Overview

SBUnix is a simple OS kernel for RISC-V architecture (RV64IMAC). The project uses a cross-compiler toolchain (`riscv64-unknown-elf-gcc`) and runs on QEMU's `virt` machine.

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
BUILD_TESTS=1 make # Build with test binaries and POSIX test suite
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

### Test Infrastructure

SBUnix has three test systems:

1. **SBUnix regression tests** (`tests/*/`) - Custom tests for kernel functionality
2. **Sortix os-test** (`tests/thirdparty/sortix/os-test/`) - POSIX compliance tests
3. **Open POSIX Test Suite** (`tests/thirdparty/open-posix-test-suite/`) - Conformance, functional, and stress tests

### Building and Running Tests

```bash
# Build with tests
BUILD_TESTS=1 make

# Run QEMU (tests do NOT auto-run on boot)
make qemu

# Inside SBUnix shell:
run-tests          # Run SBUnix regression tests
run-os-test        # Run Sortix POSIX compliance tests
run-open-posix-test # Run Open POSIX test suite
```

### Manual Testing (Original)

**For core functionality, testing is done manually:**

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

Example: `BF: Fix null pointer in allocproc`

## Current State

- `run-os-test` and `run-open-posix-test` are integrated and runnable from the shell.
- `run-os-test` uses a watchdog timeout so full runs complete deterministically.
- `run-os-test` executes tests from each suite directory to preserve relative-path assumptions.
- `posix_spawn` applies `POSIX_SPAWN_SETSIGMASK` and `POSIX_SPAWN_SETSIGDEF` before `exec`.
- Realtime signal handling supports `SIGRTMIN..SIGRTMAX` (`32..64`) for queue/wait paths.

### Known Issues (Active)

- `run-os-test` can still hit sporadic `SIGILL` in some third-party tests; the shell should remain usable afterward.
- Kernel code PTE execute-bit corruption can still occur in rare boot/test paths; current trap/boot workarounds keep the system running, but root cause is unresolved.

### QEMU Testing — Critical Notes

**`make qemu` is the primary way to run QEMU.** It builds the kernel, creates `build/disk.img`, mounts ext2 on `/mnt`, and launches QEMU with the correct flags automatically. Always prefer `make qemu` over running `qemu-system-riscv64` directly.

**`make qemu` requires a real interactive terminal** — it cannot be piped. Use `tmux`:

**Note: In sandboxed environments, tmux may need a custom socket path.** Use `-S /tmp/tmux-debug` instead of the default socket:

```bash
# Start a tmux session and run QEMU inside it (use custom socket in sandbox)
rm -f /tmp/tmux-debug 2>/dev/null
tmux -S /tmp/tmux-debug new-session -d -s qemu \
  "export PATH=/home/steven/College/Graduate_Spring_2026/CSE506/riscv-toolchain/bin:/home/steven/College/Graduate_Spring_2026/CSE506/qemu/bin:\$PATH && \
   cd /home/steven/College/Graduate_Spring_2026/CSE506/CSE506/sbunix-skeleton && \
   qemu-system-riscv64 -machine virt -bios default -kernel build/kernel.elf \
   -drive file=build/disk.img,format=raw,if=none,id=hd0 \
   -device virtio-blk-pci-non-transitional,drive=hd0 \
   -nographic 2>&1; bash"

# Wait for boot (~10s including build), then capture screen
sleep 10
tmux -S /tmp/tmux-debug capture-pane -t qemu -p | tail -5
# Should show: "sbush$"

# Send commands and capture output
tmux -S /tmp/tmux-debug send-keys -t qemu "ls" Enter
sleep 2
tmux -S /tmp/tmux-debug capture-pane -t qemu -p | tail -10

# To include test disk image (for /test access):
# Add: -drive file=build/test-disk.img,format=raw,if=none,id=hd1 -device virtio-blk-pci-non-transitional,drive=hd1

# Exit cleanly
tmux -S /tmp/tmux-debug send-keys -t qemu "poweroff" Enter
sleep 2
tmux -S /tmp/tmux-debug kill-session -t qemu
```

**Key notes about `make qemu`**:
- Uses `make qemu` in `sbunix-skeleton/` — do NOT invoke `qemu-system-riscv64` directly unless debugging a specific flag
- Automatically adds `-drive file=build/disk.img,...` so `/mnt` (ext2) is available
- Must use `-bios default` (not `-bios none`) — without SBI/OpenSBI the kernel produces no output (the Makefile handles this)
- `-nographic` maps serial to stdio (the Makefile handles this)
- QEMU binary: `/home/steven/College/Graduate_Spring_2026/CSE506/qemu/bin/qemu-system-riscv64`
- Kernel ELF: `build/kernel.elf` (entry point `0x80200000`)
- Do NOT pipe stdin/stdout to QEMU without a real tty; use tmux

### Testing Notes

Boot now includes optional regression testing — `/etc/rc` prints `SBUnix booting...`, then gives a 3-second Ctrl-C timeout before running `run-tests` automatically. Users can press Ctrl-C during the countdown to skip automatic tests and go directly to the interactive shell (using `sleep 1 || exit 0` for clean interruption handling).

Boot-test control flags in `make qemu`:
- `SKIP_BOOT_TESTS=1`: skip all automatic boot tests and start directly at the shell prompt.
- `SKIP_BOOT_PRIORITY_TESTS=1`: skip only the boot priority-test subset while keeping normal boot flow.

To run test suites from the shell prompt:
```
run-tests          # SBUnix regression tests (C tests + shell integration tests)
run-os-test        # All 1,103 Sortix POSIX compliance tests (dynamically discovered)
run-open-posix-test # All 423 compiled Open POSIX tests (2,044 in suite, 1,621 skipped)
```

**Parallel building**: Use `BUILD_TESTS=1 make -j16` for 16-way parallel os-test compilation (reduces build time to ~2-3 minutes on multi-core systems).

`/bin/run-tests` is a shell script (in `rootfs/bin/run-tests`) that runs:
1. All C test binaries: `proctest`, `schedtest`, `sigpipetest`, `fstest`, `swaptest`
2. All shell-level regression tests: Task345, Task6–Task11_14, and integration tests

`/bin/run-os-test` dynamically discovers and runs all compiled POSIX compliance tests:
- Uses sbush glob expansion to find all executable tests in `/bin/os-test/`
- Supports running individual tests: `run-os-test include/assert/assert`
- Provides summary statistics (passed/failed/total) and success rate percentage

`/bin/run-open-posix-test` dynamically discovers and runs all compiled Open POSIX tests:
- Reads from `/test/open-posix-test-manifest` (built at compile time)
- Supports running individual tests: `run-open-posix-test definitions/errno_h/4-1`
- Uses Open POSIX exit codes: 0=PASS, 1=FAIL, 2=UNRESOLVED, 4=UNSUPPORTED, 5=UNTESTED

Other useful commands to verify manually:
```
echo hello                          # Simple command
echo hello | cat                    # Single pipe
echo hello world | cat | cat        # Multi-pipe chains
sleep 2                             # Waits ~2 seconds (scheduler-based, not busy-wait)
sleep 10 (then Ctrl-C)             # Kills sleep, returns to prompt with "^C\n"
Ctrl-C at idle prompt              # Prints "^C\n", reprints prompt on new line
ls                                  # Lists: etc bin
ls /bin                             # Lists all binaries
cd /bin                             # Changes to /bin directory
cd ..                               # Returns to / (normalize_path handles ..)
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

## Documentation

Key documentation files in the project:

| File | Purpose |
|------|---------|
| `AGENTS.md` | This file - comprehensive development guide |
| `docs/os-test.md` | Sortix os-test POSIX compliance test suite usage |
| `docs/sbush-bugs.md` | Shell bug fixes and variable expansion improvements |
| `tools/analyze-os-test-failures` | Script to analyze os-test compilation failures |
| `tools/analyze-open-posix-failures` | Script to analyze open-posix compilation failures |

### Test Logs and Analysis

When building with `BUILD_TESTS=1`:
- os-test compilation logs: `build/os-test-out/sbunix/compile-logs/<suite>/<test>.log`
- os-test build summary: `build/os-test-out/sbunix/build-summary.txt`
- os-test analysis: `./tools/analyze-os-test-failures [headers|symbols|suite]`
- open-posix compilation logs: `build/open-posix-test-out/sbunix/compile-logs/<category>/<test>.log`
- open-posix build summary: `build/open-posix-test-out/sbunix/build-summary.txt`
- open-posix analysis: `./tools/analyze-open-posix-failures [headers|symbols|category]`

### Performance Notes

**Parallel test compilation**: Both test suites support full parallelization:
```bash
# Fast parallel compilation (16 cores)
BUILD_TESTS=1 make -j16

# Standard compilation
BUILD_TESTS=1 make
```

os-test build:
- **Results**: 1,103 tests compile successfully out of 5,301 total (21% success rate)

open-posix build:
- **Results**: 423 tests compile successfully out of 2,044 total (21% success rate)
