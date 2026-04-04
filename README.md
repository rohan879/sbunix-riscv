# CSE506

SBUnix course workspace for CSE506 Operating Systems.

## Repository Layout

- `sbunix-skeleton/` - the SBUnix kernel, libc, userspace programs, and build tools
- `docs/plans/` - planning documents used during development

## Setup

All build and run commands below assume you start from the repo root:

```bash
cd sbunix-skeleton
source tools/sourceme.sh
```

`tools/sourceme.sh` adds the RISC-V cross-toolchain and QEMU binaries to `PATH`.

## Build

Build the kernel and userspace:

```bash
make
```

Clean build artifacts:

```bash
make clean
```

Useful generated artifacts:

- `build/kernel.elf` - kernel image booted by QEMU
- `build/rootfs.tar` - packed tarfs root filesystem
- `build/rootfs/bin/*` - user binaries
- `build/disk.img` - disk image used by full-system QEMU

## Run The OS

Boot the full OS in QEMU:

```bash
make qemu
```

Notes:

- QEMU runs in the terminal with `-nographic`
- Exit with `Ctrl-A`, then `X`
- The system boots into `init`, runs `/etc/rc`, then drops into `sbush`

## Run A User Binary Directly

To run the shell binary in user-mode emulation instead of booting the full kernel:

```bash
qemu-riscv64 build/rootfs/bin/sbush
```

This is useful for quick shell-only experiments, but it does not test the SBUnix kernel.

## Third-Party Test Suites (Git Submodules)

The repository includes two POSIX compliance test suites as git submodules:

- **Sortix os-test** (`sbunix-skeleton/tests/thirdparty/sortix/os-test/`) — POSIX compliance tests
- **Open POSIX Test Suite** (`sbunix-skeleton/tests/thirdparty/open-posix-test-suite/posixtest/`) — IEEE conformance, functional, and stress tests

### Pulling the Submodules

After cloning the repository, initialize and fetch the submodules:

```bash
git submodule update --init --recursive
```

To later pull upstream changes for the submodules:

```bash
git submodule update --remote --recursive
```

### Building with Submodule Tests

The submodule tests are only compiled when `BUILD_TESTS=1` is set:

```bash
cd sbunix-skeleton
source tools/sourceme.sh
BUILD_TESTS=1 make -j$(nproc)
```

This compiles the os-test and open-posix suites via `tools/os-test.mk` and `tools/open-posix-test.mk`, and produces a `build/test-disk.img` containing the compiled test binaries.

### Running the Submodule Tests

Boot the OS with the test disk:

```bash
BUILD_TESTS=1 make qemu
```

Then from the `sbush` shell prompt inside QEMU:

```
run-os-test                              # Run all Sortix POSIX compliance tests
run-os-test include/assert/assert        # Run a single os-test

run-open-posix-test                      # Run all Open POSIX tests
run-open-posix-test definitions/errno_h/4-1  # Run a single open-posix test
```

### Analyzing Build Failures

Most submodule tests fail to compile due to missing headers (e.g., `pthread.h`, `sys/socket.h`). Analysis tools are provided:

```bash
./tools/analyze-os-test-failures headers    # Show missing headers for os-test
./tools/analyze-os-test-failures symbols    # Show missing symbols
./tools/analyze-os-test-failures suite      # Per-suite statistics

./tools/analyze-open-posix-failures headers   # Show missing headers for open-posix
./tools/analyze-open-posix-failures symbols   # Show missing symbols
./tools/analyze-open-posix-failures category  # Per-category statistics
```

## Automated Test Run

Run the host-side regression suite:

```bash
bash tools/run_tests.sh
```

What it does:

- runs `make clean && make`
- builds `build/disk.img` and `build/test-disk.img`
- boots the kernel in QEMU twice (Boot 1: all tests + persistence write; Boot 2: persistence verify)
- parses `[PASS]` and `[FAIL]` lines from serial output
- prints a pass/fail summary

You can also invoke the same flow with:

```bash
make test
```

## Manual Verification

Build and inspect the packed rootfs:

```bash
make
tar tf build/rootfs.tar | grep -E 'sbush|/bin/sh|etc/rc'
```

Verify a userspace binary is a RISC-V ELF:

```bash
file build/rootfs/bin/sbush
```

Disassemble a userspace binary:

```bash
riscv64-unknown-elf-objdump -d build/rootfs/bin/sbush | head -40
```

Inspect exported symbols:

```bash
riscv64-unknown-elf-nm build/rootfs/bin/sbush | grep -E 'main|parse_line|execute_pipeline'
```

## Common Workflow

Typical kernel workflow:

```bash
cd sbunix-skeleton
source tools/sourceme.sh
make
bash tools/run_tests.sh
make qemu
```

Typical quick userspace workflow:

```bash
cd sbunix-skeleton
source tools/sourceme.sh
make build/rootfs/bin/sbush
qemu-riscv64 build/rootfs/bin/sbush
```

## Submission

Create the submission bundle:

```bash
make submit
```

This exports the current tree to `/submit/sbunix`.
