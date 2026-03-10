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

## Automated Test Run

Run the host-side regression suite:

```bash
bash tools/run_tests.sh
```

What it does:

- runs `make clean && make`
- builds `build/disk.img`
- boots the kernel in QEMU
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
