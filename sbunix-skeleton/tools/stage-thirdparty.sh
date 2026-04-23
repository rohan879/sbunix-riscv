#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "usage: $0 <dest-root> [mode]" >&2
  exit 2
fi

dest_root=$1
mode=${2:-rootfs}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
thirdparty_dir=$repo_root/thirdparty

if [ -f "$repo_root/tools/sourceme.sh" ]; then
  # The TCC guest sysroot synthesis needs the cross-toolchain path even
  # when this staging script is run outside an already-sourced shell.
  # shellcheck disable=SC1091
  . "$repo_root/tools/sourceme.sh"
fi

copy_binary() {
  local src=$1
  local dst=$2
  if [ -f "$src" ]; then
    install -D -m 0755 "$src" "$dst"
  fi
}

copy_tree() {
  local src=$1
  local dst=$2
  if [ ! -d "$src" ]; then
    return 0
  fi
  mkdir -p "$dst"
  cp -a "$src"/. "$dst"/
}

copy_if_exists() {
  local src=$1
  local dst=$2
  if [ -f "$src" ]; then
    install -D -m 0644 "$src" "$dst"
  fi
}

mkdir -p "$dest_root/bin" "$dest_root/usr/lib"

# ENH: Promote the vendored workload binaries into every guest image so the
# workload tests exercise the same payloads instead of ad hoc manual copies.
copy_binary "$thirdparty_dir/bin/micropython" "$dest_root/bin/micropython"
copy_binary "$thirdparty_dir/bin/tcc" "$dest_root/bin/tcc"
copy_binary "$thirdparty_dir/bin/pdpmake" "$dest_root/bin/pdpmake"
copy_binary "$thirdparty_dir/bin/busybox" "$dest_root/bin/busybox"

if [ -f "$thirdparty_dir/bin/pdpmake" ]; then
  # ENH: Expose the imported make implementation at the conventional guest
  # path so shells and recipes can discover it through PATH without wrappers.
  ln -sfn pdpmake "$dest_root/bin/make"
fi

if [ -d "$thirdparty_dir/tcc-sysroot" ]; then
  # ENH: The vendored TCC is rebuilt with /usr/lib/tcc-relative include/lib/crt
  # paths, so promote the matching guest-visible support tree there directly.
  rm -rf "$dest_root/usr/lib/tcc"
  mkdir -p "$dest_root/usr/lib/tcc"
  copy_tree "$thirdparty_dir/tcc-sysroot/include" "$dest_root/usr/lib/tcc/include"
  copy_tree "$thirdparty_dir/tcc-sysroot/lib" "$dest_root/usr/lib/tcc/lib"
fi

# Some repo snapshots only vendor TCC headers, not the companion lib/
# runtime tree.  Guest-side `tcc hello.c -o hello` still needs startup
# objects plus libc/libgcc under /usr/lib/tcc/lib, so synthesize that view
# from the native SBUnix build outputs when the vendored lib tree is absent.
if [ ! -d "$dest_root/usr/lib/tcc/lib" ]; then
  mkdir -p "$dest_root/usr/lib/tcc/lib"
fi

if [ -d "$dest_root/usr/lib/tcc" ]; then
  copy_if_exists "$repo_root/build/libc/crt.S.o" "$dest_root/usr/lib/tcc/lib/crt1.o"
  copy_if_exists "$repo_root/build/libc.a" "$dest_root/usr/lib/tcc/lib/libc.a"

  if command -v riscv64-unknown-elf-gcc >/dev/null 2>&1; then
    libgcc_path=$(riscv64-unknown-elf-gcc -print-libgcc-file-name 2>/dev/null || true)
    crti_path=$(riscv64-unknown-elf-gcc -print-file-name=crti.o 2>/dev/null || true)
    crtn_path=$(riscv64-unknown-elf-gcc -print-file-name=crtn.o 2>/dev/null || true)
    copy_if_exists "$libgcc_path" "$dest_root/usr/lib/tcc/lib/libgcc.a"
    copy_if_exists "$crti_path" "$dest_root/usr/lib/tcc/lib/crti.o"
    copy_if_exists "$crtn_path" "$dest_root/usr/lib/tcc/lib/crtn.o"
  fi

  if [ ! -f "$dest_root/usr/lib/tcc/lib/libtcc1.a" ] && [ -f "$dest_root/usr/lib/tcc/lib/libgcc.a" ]; then
    # The vendored repo currently lacks libtcc1.a. For the simple third-
    # party integration programs, libgcc provides the helper routines TCC
    # commonly needs, and having a guest-visible archive at this path avoids
    # immediate link failure on a missing runtime support library.
    copy_if_exists "$dest_root/usr/lib/tcc/lib/libgcc.a" "$dest_root/usr/lib/tcc/lib/libtcc1.a"
  fi
fi

if [ -d "$thirdparty_dir/busybox-root" ]; then
  # ENH: Preserve the BusyBox applet link farm in a dedicated tree so the
  # guest can opt into applet-path validation without clobbering native /bin.
  rm -rf "$dest_root/usr/lib/busybox"
  mkdir -p "$dest_root/usr/lib/busybox"
  copy_tree "$thirdparty_dir/busybox-root" "$dest_root/usr/lib/busybox"
fi

# ENH: Record the staging mode for debugging serial logs when workloads fail
# after image creation but before userspace prints a clearer diagnosis.
printf '%s\n' "$mode" >"$dest_root/usr/lib/.thirdparty-stage-mode"
