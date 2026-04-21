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

## Vendored Third-Party Workloads

The repository vendors a small set of prebuilt third-party workload binaries in
`sbunix-skeleton/thirdparty/`. These are staged into the guest image by
`sbunix-skeleton/tools/stage-thirdparty.sh` during rootfs preparation.

### What Gets Included

On a fresh `make`, `make qemu`, or any build that recreates `build/rootfs`, the
following guest-visible paths are populated automatically:

- `thirdparty/bin/micropython` -> `/bin/micropython`
- `thirdparty/bin/tcc` -> `/bin/tcc`
- `thirdparty/bin/pdpmake` -> `/bin/pdpmake`
- `thirdparty/bin/busybox` -> `/bin/busybox`
- `/bin/make` -> symlink to `pdpmake`
- `thirdparty/tcc-sysroot/...` -> `/usr/lib/tcc/...`
- `thirdparty/busybox-root/...` -> `/usr/lib/busybox/...`

The Makefile path is:

- `build/rootfs/.prepared` runs `tools/stage-thirdparty.sh`
- `build/tarfs.o` is rebuilt from that prepared rootfs
- `build/kernel.elf` embeds the resulting tarfs
- `make qemu` boots that kernel image

### Refreshing After Third-Party Changes

If you only replace files under `sbunix-skeleton/thirdparty/`, a plain
`make qemu` may reuse the existing prepared rootfs. To force the vendored
payloads back into the image, use one of:

```bash
make thirdparty
```

or:

```bash
rm -f build/rootfs/.prepared build/tarfs.o
make qemu
```

### Running The Vendored Binaries In SBUnix

From the `sbush$` prompt inside QEMU:

```sh
micropython -c 'print(42)'
busybox echo busybox-ok
/usr/lib/busybox/bin/echo busybox-applet-ok

printf 'all:; echo pdpmake-ok\n' >/mnt/pd.mk
pdpmake -f /mnt/pd.mk
make -f /mnt/pd.mk

printf 'int main(void) { return 0; }\n' >/mnt/hello.c
tcc /mnt/hello.c -o /mnt/hello
/mnt/hello
```

Nested shell execution also works:

```sh
sbush -c 'micropython -c "print(42)"'
sbush -c 'busybox echo nested-ok'
sbush -c 'tcc /mnt/hello.c -o /mnt/hello && /mnt/hello'
```

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

## Running Tests

There are two ways to run the test suites: fully automated (`make test`) or manual via QEMU.

### Option 1: Automated (`make test`)

```bash
cd sbunix-skeleton
source tools/sourceme.sh
make test
```

This runs `tools/run_tests.sh`, which:

- runs `make clean && make -j1 BUILD_TESTS=1`
- compiles ~2000 Sortix os-test binaries and ~1000 Open POSIX test binaries
- builds `build/disk.img` and `build/test-disk.img`
- boots QEMU twice (Boot 1: all tests + persistence write + poweroff; Boot 2: persistence verify)
- runs the local `run-tests` regression set inside SBUnix from `sbush`, including the vendored third-party integration test for `micropython`, BusyBox, `pdpmake`, and `tcc`
- parses `[PASS]` and `[FAIL]` lines from serial output
- prints a pass/fail summary

Fully automated, no interaction needed. First run is slow (~40 min for test compilation).

### Option 2: Manual via QEMU

```bash
cd sbunix-skeleton
source tools/sourceme.sh
make clean && make BUILD_TESTS=1 qemu
```

**`BUILD_TESTS=1` is required.** Without it, no test disk is built and the test
commands inside QEMU will fail with "Test manifest not found."

First run is slow (~40 min) because it compiles all test binaries and builds a
~1.9 GB test disk image. Subsequent runs reuse the cached disk if you skip
`make clean`.

Once QEMU boots and you see `sbush$`:

```
run-tests                  # SBUnix regression tests (~1 min)
run-os-test                # Sortix os-test suite, 4071 tests (~40 min)
run-open-posix-test        # Open POSIX test suite, 989 tests (~30 min)
run-priority-tests         # Priority subset of both suites, ~1030 tests (~15 min)
```

`run-tests` is the local full regression entrypoint. It runs from inside SBUnix
via `sbush` and now includes guest-side, unmocked integration coverage for the
vendored `micropython`, BusyBox, `pdpmake`/`make`, and `tcc` workloads. There
is no new build command for this coverage; it is included automatically when you
build the documented test disk with `BUILD_TESTS=1`.

To run a single test:

```
run-os-test signal/ppoll-block-raise
run-open-posix-test conformance/interfaces/sigaction/4-12
```

Exit QEMU with `Ctrl-A`, then `X`.

### Test output protocol

Test binaries print `[PASS] <name>` or `[FAIL] <name> (details)` to stdout.
Both the automated runner and the in-QEMU commands use this format.

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

Important: the current `make submit` rule excludes `sbunix-skeleton/thirdparty/`.
That means the submission bundle will not contain the vendored workload
binaries or the staged TCC sysroot unless you change the submit rule first.
