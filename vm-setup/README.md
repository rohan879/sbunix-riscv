# CSE506 VM Environment Setup

Automated setup for the CSE506 OS-development environment on Ubuntu 24.04 x86_64.

This is done to replicate the development environment of the CSE506 course container.

## What Gets Installed

All tools install into `/opt/shared/`:

| Tool | Version | Path |
|------|---------|------|
| QEMU (riscv32 + riscv64) | 9.2.0 | `/opt/shared/qemu/` |
| RISC-V GCC (bare-metal, newlib) | 15.2.0 | `/opt/shared/riscv-toolchain/` |
| Rust (optional) | stable | `/opt/shared/rust/`, `/opt/shared/cargo/` |
| Zig (optional) | 0.13.0 | `/opt/shared/zig/` |

**NOTE:** This installation process takes a LONG TIME (upwards of 6 hours in UTM-QEMU VM).

## Prerequisites

- Ubuntu 24.04 x86_64 VM (can be downloaded [here](https://releases.ubuntu.com/noble/ubuntu-24.04.4-live-server-amd64.iso))
- `sudo` access (See the note in `setup_env.sh` about this)

## Usage

Run the setup script with `sudo`:

```bash
# Core install (APT packages + QEMU + RISC-V toolchain)
sudo bash setup_env.sh

# Skip the slow toolchain build
sudo bash setup_env.sh --skip-toolchain # Not Recommended

# Include optional languages
sudo bash setup_env.sh --with-rust --with-zig
```

## Post-Install

Log out and back in, or source the PATH manually:

```bash
source /etc/profile.d/cse506-env.sh
```

Verify the installation:

```bash
qemu-system-riscv64 --version
riscv64-unknown-elf-gcc --version
```

## Cleanup

Remove build artifacts to reclaim disk space:

```bash
sudo rm -rf /tmp/cse506-build
```

## Environment Snapshot

**NOTE:**
- This was done on the CSE506 course container.
- You DO NOT need to run this on your local VM.

`env_snapshot.sh` captures a full snapshot of the current VM environment (OS, packages, toolchain versions, `/opt/shared/` contents) for reproducibility:

```bash
./env_snapshot.sh
```

Output is saved to a timestamped directory in the current working directory.
