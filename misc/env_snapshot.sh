#!/usr/bin/env bash
# env_snapshot.sh
# Collects a reproducible snapshot of a (remote) Linux programming environment,
# including OS/kernel/toolchain versions, env vars, dotfiles, and /opt/shared
# (e.g., riscv-toolchain + qemu) inventory.
#
# Usage:
#   ./env_snapshot.sh
#   OUTDIR=~/snapshots/coursebox ./env_snapshot.sh
#
# Output:
#   A timestamped directory with text reports + optional tarballs.

set -euo pipefail

OUTDIR="${OUTDIR:-$PWD/env_snapshot_$(hostname)_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$OUTDIR"
chmod 700 "$OUTDIR"

log() { printf "[%s] %s\n" "$(date +%F\ %T)" "$*" | tee -a "$OUTDIR/00_run.log" >/dev/null; }

run() {
  # run "description" command...
  local desc="$1"; shift
  local out="$1"; shift
  log "$desc"
  {
    echo "### $desc"
    echo "\$ $*"
    echo
    "$@"
    echo
  } >> "$OUTDIR/$out" 2>&1 || {
    echo "### ERROR running: $*" >> "$OUTDIR/$out"
    echo >> "$OUTDIR/$out"
  }
}

# Write a banner
cat > "$OUTDIR/README.txt" <<EOF
Environment Snapshot
====================
Host: $(hostname)
Date: $(date -Iseconds)
User: $(whoami)
Output dir: $OUTDIR

This snapshot captures:
- OS/kernel/arch info
- Installed packages (Debian/Ubuntu or RPM-based)
- Toolchain versions (gcc/clang/make/cmake/ld, etc.)
- Environment variables, shell, dotfiles
- /opt/shared inventory, versions, and binary architectures (file(1))
EOF

# 1) OS / Kernel / Arch
run "Kernel and architecture (uname)" "01_system.txt" uname -a
run "OS release metadata (/etc/os-release)" "01_system.txt" bash -lc 'cat /etc/os-release'
run "LSB release (if available)" "01_system.txt" bash -lc 'command -v lsb_release >/dev/null && lsb_release -a || echo "lsb_release not installed"'
run "CPU info (lscpu if available)" "01_system.txt" bash -lc 'command -v lscpu >/dev/null && lscpu || echo "lscpu not installed"'
run "Memory info (free -h)" "01_system.txt" free -h
run "Disk info (df -hT)" "01_system.txt" df -hT

# 2) Environment variables and shell
run "Shell" "02_env.txt" bash -lc 'echo "$SHELL"'
run "Environment variables (printenv)" "02_env.txt" printenv
run "PATH (split)" "02_env.txt" bash -lc 'echo "$PATH" | tr ":" "\n"'

# 3) Dotfiles (record + optional archive)
DOTFILES=( "$HOME/.bashrc" "$HOME/.profile" "$HOME/.bash_aliases" "$HOME/.zshrc" "$HOME/.zprofile" "$HOME/.config" )
{
  echo "### Dotfiles presence"
  for f in "${DOTFILES[@]}"; do
    if [[ -e "$f" ]]; then
      echo "FOUND: $f"
    else
      echo "MISSING: $f"
    fi
  done
  echo
} > "$OUTDIR/03_dotfiles.txt"

# Copy readable dotfiles (avoid huge .config by default; just list it)
for f in "$HOME/.bashrc" "$HOME/.profile" "$HOME/.bash_aliases" "$HOME/.zshrc" "$HOME/.zprofile"; do
  if [[ -f "$f" ]]; then
    cp -p "$f" "$OUTDIR/$(basename "$f").copy"
  fi
done
# List ~/.config if present (don’t copy by default)
if [[ -d "$HOME/.config" ]]; then
  run "~/.config tree (top-level)" "03_dotfiles.txt" bash -lc 'ls -la ~/.config && echo && find ~/.config -maxdepth 2 -type f | head -n 200'
fi

# 4) Toolchain versions (best-effort)
TOOLS=(
  "gcc" "g++" "clang" "clang++"
  "make" "cmake" "ninja"
  "ld" "as" "ar" "ranlib" "strip"
  "python3" "pip3" "java" "javac" "node" "npm"
  "git" "gdb" "lldb" "valgrind"
  "pkg-config"
  "qemu-system-riscv64" "qemu-system-x86_64" "qemu-system-aarch64" "qemu-img"
  "riscv64-unknown-elf-gcc" "riscv64-linux-gnu-gcc" "riscv64-unknown-linux-gnu-gcc"
)

{
  echo "### Tool versions"
  for t in "${TOOLS[@]}"; do
    if command -v "$t" >/dev/null 2>&1; then
      echo
      echo "## $t"
      echo "path: $(command -v "$t")"
      # Some tools print version to stderr; capture both
      ("$t" --version 2>&1 | head -n 5) || true
      # For Java, --version may differ; try -version if needed
      if [[ "$t" == "java" || "$t" == "javac" ]]; then
        ("$t" -version 2>&1 | head -n 5) || true
      fi
    fi
  done
  echo
} > "$OUTDIR/04_tools_versions.txt"

# 5) Package inventory
if command -v dpkg >/dev/null 2>&1; then
  run "Debian/Ubuntu packages (dpkg --get-selections)" "05_packages.txt" bash -lc 'dpkg --get-selections'
  run "APT manual packages (apt-mark showmanual)" "05_packages.txt" bash -lc 'command -v apt-mark >/dev/null && apt-mark showmanual || true'
elif command -v rpm >/dev/null 2>&1; then
  run "RPM packages (rpm -qa)" "05_packages.txt" rpm -qa
else
  log "No dpkg or rpm found; skipping package inventory."
fi

# 6) /opt/shared inspection (versions + binary arch)
if [[ -d /opt/shared ]]; then
  log "Inspecting /opt/shared"
  run "/opt/shared top-level listing" "06_opt_shared.txt" bash -lc 'ls -la /opt/shared'
  # Tree (bounded)
  run "/opt/shared tree (bounded depth)" "06_opt_shared.txt" bash -lc 'find /opt/shared -maxdepth 3 -print | head -n 2000'

  # Common subdirs
  for sub in riscv-toolchain qemu; do
    if [[ -d "/opt/shared/$sub" ]]; then
      run "Listing /opt/shared/$sub" "06_opt_shared.txt" bash -lc "ls -la /opt/shared/$sub"
    fi
  done

  # Detect likely bin dirs and run file(1) on key executables
  {
    echo "### /opt/shared binary architecture checks (file)"
    echo
    # Key guesses (adjust if your layout differs)
    CANDIDATES=(
      "/opt/shared/riscv-toolchain/bin/riscv64-unknown-elf-gcc"
      "/opt/shared/riscv-toolchain/bin/riscv64-linux-gnu-gcc"
      "/opt/shared/riscv-toolchain/bin/*gcc"
      "/opt/shared/qemu/bin/qemu-system-riscv64"
      "/opt/shared/qemu/bin/qemu-img"
      "/opt/shared/qemu/bin/*qemu*"
    )
    shopt -s nullglob
    for c in "${CANDIDATES[@]}"; do
      for f in $c; do
        if [[ -x "$f" ]]; then
          echo "$f:"
          file "$f" || true
          echo
        fi
      done
    done
    shopt -u nullglob
  } > "$OUTDIR/07_opt_shared_file.txt"

  # Try running versions from /opt/shared directly if present
  {
    echo "### /opt/shared versions (best-effort)"
    echo
    if [[ -x /opt/shared/riscv-toolchain/bin/riscv64-unknown-elf-gcc ]]; then
      echo "/opt/shared/riscv-toolchain/bin/riscv64-unknown-elf-gcc --version"
      /opt/shared/riscv-toolchain/bin/riscv64-unknown-elf-gcc --version 2>&1 | head -n 10
      echo
    fi
    if [[ -x /opt/shared/qemu/bin/qemu-system-riscv64 ]]; then
      echo "/opt/shared/qemu/bin/qemu-system-riscv64 --version"
      /opt/shared/qemu/bin/qemu-system-riscv64 --version 2>&1 | head -n 10
      echo
    fi
  } > "$OUTDIR/08_opt_shared_versions.txt"

  # Optional: archive /opt/shared (may be large; off by default)
  if [[ "${ARCHIVE_OPT_SHARED:-0}" == "1" ]]; then
    log "Archiving /opt/shared (this may be large)..."
    sudo tar czf "$OUTDIR/opt_shared.tar.gz" /opt/shared 2>>"$OUTDIR/00_run.log" || {
      log "WARN: Could not archive /opt/shared (need sudo or permissions)."
    }
  else
    log "Skipping /opt/shared archive (set ARCHIVE_OPT_SHARED=1 to enable)."
  fi
else
  log "/opt/shared not found; skipping."
fi

# 7) Build behavior helpers (optional, per-project)
# If you run from inside an assignment repo, this will capture make -n / verbose traces.
if [[ -f Makefile || -f makefile || -f GNUmakefile ]]; then
  log "Makefile detected in current directory; capturing build traces."
  run "Make dry-run (make -n)" "09_build_traces.txt" bash -lc 'make -n || true'
  run "Make verbose attempt (make V=1)" "09_build_traces.txt" bash -lc 'make V=1 -n || true'
  run "Make variables dump (make -pn)" "09_build_traces.txt" bash -lc 'make -pn 2>/dev/null | head -n 2000 || true'
else
  log "No Makefile in current directory; skipping build trace capture."
fi

# 8) Summarize key paths
{
  echo "### Key executables (which)"
  for t in gcc g++ make cmake qemu-system-riscv64 riscv64-unknown-elf-gcc; do
    if command -v "$t" >/dev/null 2>&1; then
      echo "$t -> $(command -v "$t")"
    else
      echo "$t -> (not found in PATH)"
    fi
  done
  echo
  echo "### Potential PATH injections (/opt/shared references in dotfiles)"
  for f in "$HOME/.bashrc" "$HOME/.profile" "$HOME/.bash_aliases" "$HOME/.zshrc" "$HOME/.zprofile"; do
    if [[ -f "$f" ]]; then
      echo "## $f"
      grep -nE '/opt/shared|riscv|qemu|toolchain' "$f" || echo "(no matches)"
      echo
    fi
  done
} > "$OUTDIR/10_key_paths.txt"

log "Done. Snapshot written to: $OUTDIR"
echo "Snapshot complete: $OUTDIR"
