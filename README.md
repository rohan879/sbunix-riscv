# CSE506

SBUnix course workspace for CSE506 Operating Systems.

**NOTE:** 
- The `cheri` branch was an attempt to enable running the system `cheri` architecture -- which is RISC-V extended to enable detection of memory safety issues at the hardware level.
- The `vm-setup` branch contains information and instructions to setup your local virtual machine.

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

The old manually managed third-party payload directory was moved from
`sbunix-skeleton/thirdparty/` to `sbunix-skeleton/thirdparty-local/`. Usage of
`thirdparty-local` is deprecated; keep new third-party updates in the active
`thirdparty` tree unless you are intentionally testing the legacy local staging
path.

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

These payloads stay in the normal guest rootfs. They are not mirrored into
`build/testfs` or `build/test-disk.img`; the separate test disk is reserved for
`/test` binaries, manifests, logs, and persistence fixtures.

### Local Testing With Third-Party Binaries

Start local third-party binary testing by rebuilding and staging the payloads:

```bash
make thirdparty
```

`make thirdparty` builds the third-party payloads, stages them into
`build/rootfs`, rebuilds the tarfs-backed kernel image, and fails if the
required Python, make/TCC, or BusyBox payload files are missing.

Then boot SBUnix:

```bash
QEMU_MEM=128M make qemu
```

From the `sbush$` prompt, run a smoke pass over the staged binaries:

```sh
/bin/python /home/hello.py

/bin/printf 'all:\n\t/bin/tcc /home/hello.c -o /mnt/hello-make\n' > /mnt/thirdparty.mk
/bin/make -f /mnt/thirdparty.mk all
/mnt/hello-make
/bin/tcc /home/hello.c -o /mnt/hello-tcc
/mnt/hello-tcc

/bin/busybox echo busybox-ok
/usr/lib/busybox/bin/echo busybox-applet-ok
```

The temporary Makefile keeps the make/TCC smoke independent of the example
file layout while still exercising `/bin/make` and `/bin/tcc` together.

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

After cloning the repository, initialize and fetch the top-level submodules,
then initialize MicroPython's nested submodules:

```bash
git submodule update --init --recursive --depth 1
cd sbunix-skeleton/thirdparty/python
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

There are three useful test entrypoints:

- `make test` or `bash tools/run_tests.sh` for the local automated regression
- `bash tools/run_tests_capture.sh` for the one-shot full guest suite
- `BUILD_TESTS=1 make qemu` for manual interactive runs inside `sbush`

### Option 1: Automated local regression (`make test`)

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
- runs the local `run-tests` regression set inside SBUnix from `sbush`, including the vendored third-party integration test for `micropython`, BusyBox, `pdpmake`, and `tcc` across both direct exec and nested `sbush -c` dispatch
- parses `[PASS]` and `[FAIL]` lines from serial output
- prints a pass/fail summary

Fully automated, no interaction needed. First run is slow (~40 min for test compilation).
This automated path validates the local `run-tests` regression set plus the
two-boot persistence flow. It builds the Sortix and Open POSIX suites into the
test disk, but it does not automatically execute `run-os-test` or
`run-open-posix-test`; those remain explicit in-guest commands from `sbush`.
The vendored `micropython`, BusyBox, `pdpmake`, and `tcc` coverage runs against
the rootfs-installed `/bin/*` and `/usr/lib/*` payloads, not copies on the test
disk. This avoids `mkext2img`'s ext2-lite large-file limit on the staged TCC
archives.

Use this path when you want the standard host-side regression quickly. If you
need the full guest-visible suite in one command, use the captured runner below.

### Option 2: Targeted third-party workload integration only

To run only the in-code vendored workload integration test, use the targeted
QEMU runner:

```bash
cd sbunix-skeleton
source tools/sourceme.sh
unset THIRDPARTY_CI_SKIP_BUSYBOX
MAKE_JOBS=4 REQUIRE_THIRDPARTY_PAYLOADS=1 \
  tools/run_targeted_qemu.sh "/test/bin/thirdpartytest"
```

This compiles the local `tests/thirdpartytest` binary into the lightweight
`build/test-disk-core.img` and does not compile or package the Sortix os-test
or Open POSIX suites. It exercises MicroPython, BusyBox, `pdpmake`, TCC, and
nested `sbush -c` dispatch against the rootfs-staged third-party payloads.

The settings are:

- Do not set `BUILD_TESTS=1`; the targeted runner defaults to
  `build/test-disk-core.img`.
- Set `REQUIRE_THIRDPARTY_PAYLOADS=1` to fail early if the rootfs payload is
  missing.
- Leave `THIRDPARTY_CI_SKIP_BUSYBOX` unset or `0` when you want BusyBox and the
  nested `sbush` third-party checks to run. With `THIRDPARTY_CI_SKIP_BUSYBOX=1`,
  BusyBox is intentionally absent and those portions skip.

### Option 3: One-shot full guest suite (`tools/run_tests_capture.sh`)

```bash
cd sbunix-skeleton
source tools/sourceme.sh
bash tools/run_tests_capture.sh
```

This wrapper is the documented one-command path for the full suite. It:

- rebuilds with `BUILD_TESTS=1`
- builds both `build/disk.img` and `build/test-disk.img`
- boots QEMU twice
- lets `/etc/rc` run the local `run-tests` regression on Boot 1
- waits for the first `sbush$` prompt, then runs `run-os-test`,
  `run-open-posix-test`, and `/test/bin/persisttest`
- boots a second time so persistence verification runs and powers off
- preserves raw per-boot logs and a host status file

By default the artifacts are written to:

```bash
/home/adebayo/test-findings/code-review-08-artifacts/final
```

You can override the output directory and timeout:

```bash
SBUNIX_RUN_TESTS_CAPTURE_DIR=/tmp/sbunix-full TIMEOUT=10800 bash tools/run_tests_capture.sh
```

Important notes:

- This is much slower than `make test` because it executes the local regression,
  the full Sortix os-test suite, the full Open POSIX suite, and the two-boot
  persistence flow in a single run.
- The default timeout is `7200` seconds.
- The wrapper assumes `BUILD_TESTS=1` support is available and will rebuild the
  test disk itself; you do not need to run `make BUILD_TESTS=1` separately.

### Option 4: Manual via QEMU

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
vendored `micropython`, BusyBox, `pdpmake`/`make`, and `tcc` workloads across
both direct execution and nested `sbush -c` command dispatch. There is no new
build command for this coverage; it is included automatically when you build
the documented test disk with `BUILD_TESTS=1`. Those workload binaries are read
from the normal guest rootfs, while the separate test disk provides `/test`
binaries, manifests, logs, and persistence fixtures only.

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
# use bash tools/run_tests_capture.sh when you want the full guest suite
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
