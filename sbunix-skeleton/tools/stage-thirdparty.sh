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
