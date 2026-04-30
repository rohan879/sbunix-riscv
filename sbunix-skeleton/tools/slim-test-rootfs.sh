#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ]; then
  echo "usage: $0 <rootfs> <testfs> [testfs...]" >&2
  exit 2
fi

rootfs=$1
shift

copy_payload() {
  local src=$1
  local rel=$2
  shift 2

  local dstroot
  for dstroot in "$@"; do
    mkdir -p "$dstroot/$(dirname "$rel")"
    if [ -d "$src" ] && [ ! -L "$src" ]; then
      rm -rf "$dstroot/$rel"
      mkdir -p "$dstroot/$rel"
      cp -a "$src"/. "$dstroot/$rel"/
    elif [ -e "$src" ] || [ -L "$src" ]; then
      rm -f "$dstroot/$rel"
      cp -a "$src" "$dstroot/$rel"
    fi
  done
}

link_to_test() {
  local path=$1
  local target=$2
  rm -rf "$path"
  ln -s "$target" "$path"
}

stage_bin() {
  local name=$1
  local src="$rootfs/bin/$name"
  shift

  [ -f "$src" ] || return 0
  copy_payload "$src" "bin/rootfs/$name" "$@"
  link_to_test "$src" "/test/bin/rootfs/$name"
}

mkdir -p "$rootfs/bin" "$rootfs/usr/lib"

for dstroot in "$@"; do
  mkdir -p "$dstroot/bin/rootfs" "$dstroot/usr/lib"
done

for path in "$rootfs"/bin/*; do
  [ -f "$path" ] || continue
  name=$(basename "$path")
  # BF: Keep shell entrypoints in tarfs; third-party make recipes exec
  # /bin/sh before a test-disk symlink can safely stand in for sbush.
  # Keep /bin/echo resident too because realpath canaries assert the
  # canonical target is /bin/echo, not the slimmed /test/bin/rootfs copy.
  case "$name" in
    init|sbush|sh|echo|run-tests|run-os-test|run-open-posix-test|run-priority-tests)
      continue
      ;;
  esac
  stage_bin "$name" "$@"
done

if [ -d "$rootfs/usr/lib/tcc" ] && [ ! -L "$rootfs/usr/lib/tcc" ]; then
  copy_payload "$rootfs/usr/lib/tcc" "usr/lib/tcc" "$@"
  link_to_test "$rootfs/usr/lib/tcc" "/test/usr/lib/tcc"
fi

if [ -d "$rootfs/usr/lib/busybox" ] && [ ! -L "$rootfs/usr/lib/busybox" ]; then
  copy_payload "$rootfs/usr/lib/busybox" "usr/lib/busybox" "$@"
  link_to_test "$rootfs/usr/lib/busybox" "/test/usr/lib/busybox"
fi

printf '%s\n' "test-slim" >"$rootfs/usr/lib/.thirdparty-stage-mode"
