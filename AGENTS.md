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
make qemu        # Build if needed and run kernel in QEMU (exit with Ctrl-A X)
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

1. **Pre-existing issue**: Kernel syscall numbers (`kernel/include/syscall.h`) do not match libc's Linux RISC-V ABI numbers - this causes "Unknown syscall" errors
2. **No automated tests**: All testing is manual
3. **Development branches**: Work on separate branches, test locally before merging
4. **Cross-compilation**: All binaries must be built for RISC-V target
