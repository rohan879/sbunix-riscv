# CSE506 SBUnix Workspace

This repository contains the SBUnix teaching operating system for CSE506.
The active project lives under `sbunix-skeleton/`.

SBUnix targets RV64IMAC/RV64 on QEMU's `virt` machine. The normal QEMU path
boots through OpenSBI (`-bios default`), loads `build/kernel.elf`, embeds a
tarfs root filesystem, and can attach ext2 block images for `/mnt` and `/test`.
User programs are statically linked against the in-tree libc and are discovered
from `bin/*/*.c` and `bin/*/*.rs` by the Makefile.

Current build, run, and validation work should start from `sbunix-skeleton/`.

The other branches:
- The `cheri` branch was an attempt to enable running the system `cheri` architecture -- which is RISC-V extended to enable detection of memory safety issues at the hardware level.
- The `vm-setup` branch contains information and instructions to setup your local virtual machine.

## Repository Layout

- `sbunix-skeleton/kernel/` - RV64 kernel, VM, VFS, device drivers, traps, and syscall handlers
- `sbunix-skeleton/libc/` - custom freestanding/static C library and headers
- `sbunix-skeleton/bin/` - auto-discovered guest programs, including `sbush`
- `sbunix-skeleton/rootfs/` - files copied into the guest tarfs before packaging
- `sbunix-skeleton/tests/` - local regression tests plus third-party POSIX suite sources
- `sbunix-skeleton/thirdparty/` - active workload source/submodule tree for MicroPython, TCC, pdpmake, and BusyBox
- `sbunix-skeleton/thirdparty-local/` - legacy local payloads retained for reference
- `sbunix-skeleton/tools/` - build, staging, QEMU, and analysis helpers
- `sbunix-skeleton/docs/` - project notes and validation plans

## Setup

Run build commands from `sbunix-skeleton/` after sourcing the toolchain path:

```bash
cd sbunix-skeleton
source tools/sourceme.sh
```

`tools/sourceme.sh` adds the RISC-V cross-compiler, QEMU, Rust, and Zig tools
expected by the Makefile.

## Build

Build the kernel, libc, userspace, embedded tarfs, and normal disk image:

```bash
make
```

Clean generated artifacts:

```bash
make clean
```

Useful outputs:

- `build/kernel.elf` - kernel image loaded by QEMU
- `build/rootfs.tar` - tarfs embedded into the kernel image
- `build/rootfs/` - host-side staged rootfs before tarfs packaging
- `build/disk.img` - ext2 image normally mounted as `/mnt`
- `build/test-disk.img` - optional test image mounted as `/test` when built with `BUILD_TESTS=1`

## Run

Boot SBUnix in QEMU:

```bash
make qemu
```

This target builds what it needs, creates `build/disk.img`, launches
`qemu-system-riscv64` with the project defaults, and drops into `sbush` after
`init` runs `/etc/rc`.

Debug with QEMU's GDB stub:

```bash
make qemu-gdb
```

Then connect from another shell with `riscv64-unknown-elf-gdb build/kernel.elf`
and `target remote :1234`.

For shell-only experiments outside the SBUnix kernel:

```bash
qemu-riscv64 build/rootfs/bin/sbush
```

## Runtime Shape

The guest root starts from the embedded tarfs. During normal QEMU boots,
`build/disk.img` is available through virtio-blk and mounted by the OS as the
writable `/mnt` filesystem. Test builds can also attach `build/test-disk.img`,
which carries `/test` binaries, manifests, logs, and persistence fixtures.

`/etc/rc` sets `HOME=/`, `PATH=/bin:/usr/lib/busybox/bin`, and
`SHELL=/bin/sbush`. If `/bin/run-tests` is present, boot runs the SBUnix
regression suite unless the image was built with the skip marker. Use this for
manual test-disk boots when you want a prompt immediately:

```bash
BUILD_TESTS=1 SKIP_BOOT_TESTS=1 make qemu
```

## Capabilities

Current SBUnix implements a teaching-kernel subset, not a complete Unix:

- RV64IMAC S-mode kernel on QEMU `virt`, booted through OpenSBI
- Sv39 virtual memory with lazy allocation, copy-on-write fork, `mmap`, `mprotect`, and `mremap`
- W^X kernel mappings, centralized `pte_update()` checks, and kernel PTE diagnostics
- Linux RISC-V syscall numbering for the implemented syscall surface
- Process creation, exec, wait, process groups, sessions, basic job-control support, and narrow `CLONE_VM`/vfork-style support
- Signals 1-64, including realtime queue/wait paths and `SA_SIGINFO` delivery paths
- UART/TTY, PLIC, RTC/timekeeping, timers, and virtio-blk
- VFS with tarfs, ext2, ramfs, devfs, procfs, and sysfs mounts
- Custom libc with stdio, dirent, signals, spawn, mmap wrappers, regex/fnmatch/glob, time, and common POSIX headers
- `sbush` plus core utilities such as `cat`, `cp`, `date`, `echo`, `grep`, `ls`, `mkdir`, `mount`, `ps`, `rm`, `sleep`, `sort`, `test`, `touch`, `uname`, `wc`, and the test runners

## Third-Party Workloads

There are two distinct third-party areas:

- Active workload sources and submodules live under `sbunix-skeleton/thirdparty/`.
- Legacy local payloads live under `sbunix-skeleton/thirdparty-local/` and are not the preferred update path.

Do not assume checked-in workload payloads are copied straight into the guest.
The current deterministic path is:

1. `make thirdparty` builds disposable workload payloads.
2. Generated files land under `build/thirdparty-payload`.
3. `tools/stage-thirdparty.sh` promotes the selected payload tree into the staged guest root.

The staged guest paths include `/bin/micropython`, `/bin/python`, `/bin/tcc`,
`/bin/pdpmake`, `/bin/make`, `/usr/lib/tcc`, and, when enabled,
`/bin/busybox` plus `/usr/lib/busybox/bin/*`.

Build and stage the workload payloads with:

```bash
make thirdparty
```

If you only changed generated payload content and a prepared rootfs already
exists, force restaging through `make thirdparty` rather than relying on an
unchanged `make qemu` invocation to notice it.

## Third-Party POSIX Suites

The POSIX test suites are source submodules under `tests/thirdparty/`:

- Sortix os-test: `tests/thirdparty/sortix/os-test/`
- Open POSIX Test Suite: `tests/thirdparty/open-posix-test-suite/posixtest/`

Initialize submodules after cloning:

```bash
git submodule update --init --recursive --depth 1
```

The suites are compiled only when `BUILD_TESTS=1` is set. Their compiled
payloads are staged on the test disk, not in the normal rootfs:

- Local regression binaries: `/test/bin/<name>`
- Sortix os-test binaries: `/test/bin/os-test`
- Sortix manifest: `/test/os-test-manifest`
- Open POSIX binaries: `/test/bin/open-posix-test`
- Open POSIX manifest: `/test/open-posix-test-manifest`

The guest entrypoints are:

```sh
run-tests
run-os-test
run-open-posix-test
run-priority-tests
```

Targeted examples from the guest prompt:

```sh
/test/bin/sigtest
run-os-test signal/ppoll-block-raise
run-open-posix-test conformance/interfaces/sigaction/4-12
```

## Testing

This README update is documentation-only. The commands below describe the
supported paths; running the expensive `BUILD_TESTS=1` workflows is a separate
validation decision.

### Host Regression Runner

```bash
make test
```

The Makefile default is `TIMEOUT=1800` and the target invokes:

```bash
TIMEOUT=$(TIMEOUT) bash tools/run_tests.sh
```

Direct script usage defaults to `TIMEOUT=900`:

```bash
bash tools/run_tests.sh
```

`tools/run_tests.sh` sources `tools/sourceme.sh`, runs `make clean`, builds
with `BUILD_TESTS=1`, creates `build/disk.img` and `build/test-disk.img`,
boots QEMU twice, validates persistence, and parses guest `[PASS]`/`[FAIL]`
lines. It runs the local `run-tests` regression path; the full Sortix and Open
POSIX runners remain explicit guest commands unless driven by another wrapper.

### Captured Full Guest Runner

```bash
bash tools/run_tests_capture.sh
```

The `run_tests_capture` wrapper defaults to `TIMEOUT=28800`. It rebuilds with
`BUILD_TESTS=1`, creates both disk images, captures per-boot logs, lets boot
run the local regression suite, then drives `run-os-test`,
`run-open-posix-test`, and persistence checks from the guest shell.

Override its output directory or timeout with environment variables:

```bash
SBUNIX_RUN_TESTS_CAPTURE_DIR=/tmp/sbunix-full TIMEOUT=28800 bash tools/run_tests_capture.sh
```

### Manual Test-Disk Boot

```bash
BUILD_TESTS=1 SKIP_BOOT_TESTS=1 make qemu
```

From `sbush$`, run one command at a time for clear serial logs:

```sh
run-tests
run-priority-tests
run-os-test
run-open-posix-test
```

### Targeted QEMU Runner

For focused guest checks without building the full third-party POSIX suites:

```bash
tools/run_targeted_qemu.sh "/test/bin/mprotecttest"
```

The targeted runner uses `build/test-disk-core.img` by default and fails if
the serial log contains failure diagnostics such as `kernel PTE repair` or
`pte-update violation`.

## Manual Inspection

After a normal build:

```bash
tar tf build/rootfs.tar | grep -E 'sbush|bin/sh|etc/rc'
file build/rootfs/bin/sbush
riscv64-unknown-elf-objdump -d build/rootfs/bin/sbush | head -40
riscv64-unknown-elf-nm build/rootfs/bin/sbush | grep -E 'main|parse_line|execute_pipeline'
```

## PTE Caveat

Rare kernel PTE execute-bit corruption remains a known caveat; the root-cause status is not conclusively closed. The current tree has W^X kernel mappings,
centralized `pte_update()` invariants, optional `PTE_DEBUG_FAILFAST`, boot,
timer, virtio, and trap repair/audit hooks, and `kernel_wx_audit()`.

`docs/plan-03.md` and `docs/plan-03-audit.md` record prior no-repair
validation evidence. Fresh validation should still check that serial logs do
not contain:

- `kernel PTE repair`
- `timer: repaired`
- `pte-update violation`
- `boot: repaired kernel code PTE permissions`
- `boot: WARNING kernel W^X`

## Current Limitations

- The kernel is oriented around single-hart execution.
- Some paths still use placeholder or teaching-kernel locking behavior.
- Resident-page reclaim/swap accounting exists, but there is no active production-quality reclaim path.
- `madvise` validates ranges and returns success without policy action.
- `CLONE_VM` is intentionally limited and is not a general thread implementation.
- POSIX conformance is broadening through tests, but unsupported syscalls and edge semantics remain.

## Submission

Create the submission bundle:

```bash
make submit
```

The submit rule exports to `/submit/sbunix` and excludes generated build
outputs, VCS metadata, and the active third-party source tree.
