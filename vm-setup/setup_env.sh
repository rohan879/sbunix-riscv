#!/usr/bin/env bash
#
# setup_env.sh — Replicate the CSE506 OS-development environment
#
# Installs into /opt/shared/:
#   qemu/             QEMU 9.2.0  (riscv32 + riscv64 targets)
#   riscv-toolchain/  riscv64-unknown-elf-gcc 15.2.0  (newlib, bare-metal)
#   rust/ + cargo/    Rust stable toolchain   (optional, --with-rust)
#   zig/              Zig 0.13.0              (optional, --with-zig)
#   bin/              (empty helper dir)
#
# Also installs required APT packages and configures PATH.
#
# Usage:  sudo bash setup_env.sh                    (core: APT + QEMU + toolchain)
#         sudo bash setup_env.sh --skip-toolchain   (skip slow RISC-V build)
#         sudo bash setup_env.sh --with-rust         (also install Rust)
#         sudo bash setup_env.sh --with-zig          (also install Zig)
#         sudo bash setup_env.sh --with-rust --with-zig  (install everything)
#
# Meant to run on Ubuntu 24.04 x86_64.
# -----------------------------------------------------------------------
# 
# NOTE: 
# To disable sudo password prompts for the current user (e.g. in a VM), you can run:
# sudo sh -c 'echo "$USER ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/$USER'
# 
# To re-enable password prompts, simply remove that file:
# sudo rm /etc/sudoers.d/$USER
# 
# WARNING (for NOTE above): 
# This is EXTREMELY dangerous -- only use this within a controlled environment (e.g. a VM) 
# where you understand the security implications. It effectively gives full root access to any 
# process running as your user.

set -euo pipefail

die()   { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

SKIP_TOOLCHAIN=0
WITH_RUST=0
WITH_ZIG=0
for arg in "$@"; do
    case "$arg" in
        --skip-toolchain) SKIP_TOOLCHAIN=1 ;;
        --with-rust)      WITH_RUST=1 ;;
        --with-zig)       WITH_ZIG=1 ;;
        --help|-h)
            echo "Usage: sudo bash $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --skip-toolchain   Skip the RISC-V cross-compiler build"
            echo "  --with-rust        Also install the Rust toolchain"
            echo "  --with-zig         Also install the Zig compiler"
            echo "  --help, -h         Show this help"
            exit 0
            ;;
        *)  die "Unknown option: $arg (try --help)" ;;
    esac
done

# ---------- tunables ----------------------------------------------------
INSTALL_PREFIX="/opt/shared"
QEMU_VERSION="9.2.0"
RVTC_GCC_VERSION="15.2.0"         # target GCC inside riscv-gnu-toolchain
ZIG_VERSION="0.13.0"
BUILD_DIR="/tmp/cse506-build"
NPROC=$(nproc)
# -----------------------------------------------------------------------

info()  { printf '\n\033[1;34m>>> %s\033[0m\n' "$*"; }
ok()    { printf '\033[1;32m    ✓ %s\033[0m\n' "$*"; }
warn()  { printf '\033[1;33m    ! %s\033[0m\n' "$*"; }

[[ $(id -u) -eq 0 ]] || die "Run this script with sudo (or as root)."

mkdir -p "$INSTALL_PREFIX/bin" "$BUILD_DIR"

# ===== 1. APT packages ==================================================
info "Installing APT packages"

export DEBIAN_FRONTEND=noninteractive

apt-get update -qq

# Packages explicitly marked "manual" in the snapshot, plus build deps
# for QEMU and the RISC-V toolchain.
apt-get install -y -qq \
    build-essential \
    git curl wget \
    strace tmux vim htop screen \
    python3 python3-serial python3-pip \
    jq bc patch \
    \
    libfdt-dev libfdt1 \
    libpixman-1-dev libpixman-1-0 \
    libslirp-dev libslirp0 \
    libglib2.0-dev \
    libncurses-dev \
    libssl-dev \
    \
    autoconf automake autotools-dev \
    libmpc-dev libmpfr-dev libgmp-dev \
    gawk texinfo libtool patchutils zlib1g-dev \
    libexpat1-dev \
    ninja-build pkg-config \
    flex bison \
    \
    libusb-1.0-0-dev \
    2>&1 | tail -3

ok "APT packages installed"

# ===== 2. QEMU 9.2.0 ====================================================
if [[ -x "$INSTALL_PREFIX/qemu/bin/qemu-system-riscv64" ]]; then
    ok "QEMU already installed — skipping"
else
    info "Building QEMU $QEMU_VERSION (RISC-V targets only)"

    cd "$BUILD_DIR"
    QEMU_TAR="qemu-${QEMU_VERSION}.tar.xz"
    if [[ ! -f "$QEMU_TAR" ]]; then
        wget -q "https://download.qemu.org/qemu-${QEMU_VERSION}.tar.xz" -O "$QEMU_TAR"
    fi
    tar xf "$QEMU_TAR"
    cd "qemu-${QEMU_VERSION}"

    ./configure \
        --prefix="$INSTALL_PREFIX/qemu" \
        --target-list=riscv64-softmmu,riscv32-softmmu \
        --disable-werror \
        --enable-slirp

    make -j"$NPROC"
    make install

    ok "QEMU $QEMU_VERSION installed to $INSTALL_PREFIX/qemu"
fi

# ===== 3. RISC-V bare-metal toolchain (riscv64-unknown-elf-gcc) ==========
if [[ -x "$INSTALL_PREFIX/riscv-toolchain/bin/riscv64-unknown-elf-gcc" ]]; then
    ok "RISC-V toolchain already installed — skipping"
elif [[ $SKIP_TOOLCHAIN -eq 1 ]]; then
    warn "Skipping RISC-V toolchain build (--skip-toolchain)"
else
    info "Building RISC-V bare-metal toolchain (newlib, GCC $RVTC_GCC_VERSION)"
    warn "This is the longest step — it will use $NPROC parallel jobs."

    cd "$BUILD_DIR"
    if [[ ! -d riscv-gnu-toolchain ]]; then
        git clone --depth 1 https://github.com/riscv-collab/riscv-gnu-toolchain.git
    fi
    cd riscv-gnu-toolchain

    ./configure --prefix="$INSTALL_PREFIX/riscv-toolchain" \
                --with-arch=rv64gc \
                --with-abi=lp64d

    # "newlib" target produces riscv64-unknown-elf-*
    make -j"$NPROC"

    ok "RISC-V toolchain installed to $INSTALL_PREFIX/riscv-toolchain"
fi

# ===== 4. Rust toolchain (optional) =======================================
if [[ $WITH_RUST -eq 0 ]]; then
    warn "Skipping Rust (pass --with-rust to install)"
elif [[ -x "$INSTALL_PREFIX/cargo/bin/rustc" ]]; then
    ok "Rust already installed — skipping"
else
    info "Installing Rust toolchain (stable)"

    export RUSTUP_HOME="$INSTALL_PREFIX/rust"
    export CARGO_HOME="$INSTALL_PREFIX/cargo"

    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
        | sh -s -- -y --no-modify-path --default-toolchain stable

    ok "Rust installed to $INSTALL_PREFIX/rust + $INSTALL_PREFIX/cargo"
fi

# ===== 5. Zig (optional) ==================================================
if [[ $WITH_ZIG -eq 0 ]]; then
    warn "Skipping Zig (pass --with-zig to install)"
elif [[ -x "$INSTALL_PREFIX/zig/zig" ]]; then
    ok "Zig already installed — skipping"
else
    info "Installing Zig $ZIG_VERSION"

    cd "$BUILD_DIR"
    ZIG_TAR="zig-linux-x86_64-${ZIG_VERSION}.tar.xz"
    if [[ ! -f "$ZIG_TAR" ]]; then
        wget -q "https://ziglang.org/download/${ZIG_VERSION}/zig-linux-x86_64-${ZIG_VERSION}.tar.xz" \
             -O "$ZIG_TAR"
    fi
    mkdir -p "$INSTALL_PREFIX/zig"
    tar xf "$ZIG_TAR" --strip-components=1 -C "$INSTALL_PREFIX/zig"

    ok "Zig $ZIG_VERSION installed to $INSTALL_PREFIX/zig"
fi

# ===== 6. PATH configuration =============================================
info "Configuring PATH for all users"

PROFILE_SNIPPET="/etc/profile.d/cse506-env.sh"
{
    echo '# CSE506 OS development environment'
    echo 'export PATH="/opt/shared/riscv-toolchain/bin:/opt/shared/qemu/bin:/opt/shared/bin:$PATH"'
    if [[ -x "$INSTALL_PREFIX/cargo/bin/rustc" ]]; then
        echo 'export RUSTUP_HOME="/opt/shared/rust"'
        echo 'export CARGO_HOME="/opt/shared/cargo"'
        echo 'export PATH="/opt/shared/cargo/bin:$PATH"'
    fi
} > "$PROFILE_SNIPPET"
chmod 644 "$PROFILE_SNIPPET"

ok "Wrote $PROFILE_SNIPPET"

# Also inject into the calling user's .bashrc if not already present
CALLER_HOME="${SUDO_USER:+$(eval echo "~$SUDO_USER")}"
CALLER_HOME="${CALLER_HOME:-$HOME}"
BASHRC="$CALLER_HOME/.bashrc"
MARKER="# CSE506-env"

if [[ -f "$BASHRC" ]] && ! grep -qF "$MARKER" "$BASHRC"; then
    cat >> "$BASHRC" <<RCEOF

$MARKER
source /etc/profile.d/cse506-env.sh
RCEOF
    ok "Appended source line to $BASHRC"
else
    ok "$BASHRC already has CSE506 PATH entries"
fi

# ===== 7. Cleanup (optional) =============================================
info "Build artifacts are in $BUILD_DIR"
echo "    Run 'sudo rm -rf $BUILD_DIR' to reclaim disk space after verifying."

# ===== Summary ===========================================================
info "Installation summary"
echo ""
echo "  QEMU:       $INSTALL_PREFIX/qemu/bin/qemu-system-riscv64"
echo "  RV64 GCC:   $INSTALL_PREFIX/riscv-toolchain/bin/riscv64-unknown-elf-gcc"
[[ -x "$INSTALL_PREFIX/cargo/bin/rustc" ]] && echo "  Rust:       $INSTALL_PREFIX/cargo/bin/rustc"
[[ -x "$INSTALL_PREFIX/zig/zig" ]]         && echo "  Zig:        $INSTALL_PREFIX/zig/zig"
echo ""
echo "  Log out and back in (or run 'source /etc/profile.d/cse506-env.sh')"
echo "  to pick up the new PATH."
echo ""
