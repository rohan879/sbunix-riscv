#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
source_root=$repo_root/thirdparty
src_dir=${THIRDPARTY_SRC_DIR:-$repo_root/build/thirdparty-src}
payload_dir=${THIRDPARTY_PAYLOAD_DIR:-$repo_root/build/thirdparty-payload}
work_top=$repo_root/build/thirdparty-work
jobs=${MAKE_JOBS:-$(nproc 2>/dev/null || echo 4)}
skip_busybox=0

if [ "${THIRDPARTY_CI_SKIP_BUSYBOX:-0}" = "1" ]; then
  skip_busybox=1
fi

case $src_dir in
  /*) ;;
  *) src_dir=$repo_root/$src_dir ;;
esac

case $payload_dir in
  /*) ;;
  *) payload_dir=$repo_root/$payload_dir ;;
esac

require_build_path() {
  local path=$1
  case $path in
    "$repo_root"/build/*) ;;
    *)
      echo "build-thirdparty-ci: refusing to remove non-build path: $path" >&2
      exit 1
      ;;
  esac
}

if [ ! -d "$source_root" ]; then
  echo "build-thirdparty-ci: missing source tree: $source_root" >&2
  exit 1
fi

require_build_path "$src_dir"
require_build_path "$payload_dir"
require_build_path "$work_top"

rm -rf "$src_dir" "$payload_dir" "$work_top"
mkdir -p "$src_dir" "$payload_dir/bin" "$work_top/build/libc" "$work_top/libc"

if [ ! -f "$repo_root/build/libc/crt.S.o" ] || [ ! -f "$repo_root/build/libc.a" ]; then
  # BF: CI invokes this script directly after make clean, so materialize
  # the SBUnix libc inputs before building disposable third-party copies.
  $(command -v make) -C "$repo_root" build/libc/crt.S.o build/libc.a
fi

copy_dir() {
  local src=$1
  local dst=$2
  if [ ! -d "$src" ]; then
    return 0
  fi
  mkdir -p "$dst"
  cp -a "$src"/. "$dst"/
}

copy_required_file() {
  local src=$1
  local dst=$2
  if [ ! -f "$src" ]; then
    echo "build-thirdparty-ci: missing source file: $src" >&2
    exit 1
  fi
  install -D -m 0644 "$src" "$dst"
}

copy_submodule_clean() {
  local name=$1
  local src=$source_root/$name
  local dst=$src_dir/$name

  if [ ! -d "$src" ]; then
    echo "build-thirdparty-ci: missing submodule directory: $src" >&2
    exit 1
  fi

  mkdir -p "$dst"
  cp -a "$src"/. "$dst"/
  find "$dst" -name .git -prune -exec rm -rf {} +
}

for file in Makefile bb_printf.c busybox-sbunix.patch busybox_defconfig \
            compat.c dummy.c python-sbunix.patch setjmp.S softfloat.c; do
  copy_required_file "$source_root/$file" "$src_dir/$file"
done

copy_dir "$source_root/include" "$src_dir/include"
copy_dir "$source_root/python-examples" "$src_dir/python-examples"
copy_dir "$source_root/tcc-examples" "$src_dir/tcc-examples"

submodules=(python tcc make)
if [ "$skip_busybox" = "0" ]; then
  submodules=(busybox "${submodules[@]}")
fi

for submodule in "${submodules[@]}"; do
  copy_submodule_clean "$submodule"
done

# BF: The copied TCC config generator must match the guest staging path used
# by tools/stage-thirdparty.sh; editing only the disposable copy keeps source
# submodules clean while producing reproducible CI payloads.
sed -i "s|^TOP[[:space:]]*:=[[:space:]].*|TOP := $work_top|" "$src_dir/Makefile"
perl -0pi -e 's#define CONFIG_TCCDIR "/lib/tcc"#define CONFIG_TCCDIR "/usr/lib/tcc"#g;
              s#define CONFIG_TCC_SYSINCLUDEPATHS "/lib/tcc/include"#define CONFIG_TCC_SYSINCLUDEPATHS "/usr/lib/tcc/include"#g;
              s#define CONFIG_TCC_LIBPATHS "/lib"#define CONFIG_TCC_LIBPATHS "/usr/lib/tcc/lib"#g;
              s#define CONFIG_TCC_CRTPREFIX "/lib"#define CONFIG_TCC_CRTPREFIX "/usr/lib/tcc/lib"#g;
              s/ environ\.c//g' \
  "$src_dir/Makefile"

if [ "$skip_busybox" = "0" ]; then
  # BF: SBUnix libc exposes an itoa(value, buf, base) extension, while BusyBox
  # has a private itoa(int) helper.  Rename only the disposable BusyBox copy.
  find "$src_dir/busybox" -type f \( -name '*.[ch]' -o -name '*.src' \) \
    -exec perl -0pi -e 's/\bitoa\b/bb_itoa/g' {} +
fi

# BF: BusyBox env uses clearenv(), which SBUnix libc does not export.  Add the
# prototype and weak shim only to the disposable CI source tree.
printf '\nint clearenv(void);\n' >>"$src_dir/include/stdlib.h"
printf '\nchar *strchrnul(const char *s, int c);\nint strverscmp(const char *a, const char *b);\n' >>"$src_dir/include/string.h"
printf '\nint sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask);\n' >>"$src_dir/include/sched.h"
cat >>"$src_dir/compat.c" <<'EOF'

W int clearenv(void) {
	extern char **environ;
	if (environ)
		environ[0] = NULL;
	return 0;
}

#include <sched.h>

W char *strchrnul(const char *s, int c) {
	while (*s && *s != (char)c)
		s++;
	return (char *)s;
}

W int strverscmp(const char *a, const char *b) {
	return strcmp(a, b);
}

W int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask) {
	(void)pid;
	if (!mask || cpusetsize < sizeof(cpu_set_t)) {
		errno = EINVAL;
		return -1;
	}
	memset(mask, 0, cpusetsize);
	mask->__bits[0] = 1;
	return 0;
}
EOF

ln -s "$repo_root/libc/include" "$work_top/libc/include"
ln -s "$repo_root/build/libc/crt.S.o" "$work_top/build/libc/crt.S.o"
ln -s "$repo_root/build/libc.a" "$work_top/build/libc.a"

make_ci() {
  $(command -v make) -C "$src_dir" "$@"
}

build_targets=(python tcc-install pdpmake)
if [ "$skip_busybox" = "0" ]; then
  build_targets=(busybox-install "${build_targets[@]}")
fi

make_ci -j"$jobs" "${build_targets[@]}"

copy_executable() {
  local src=$1
  local dst=$2
  if [ ! -x "$src" ]; then
    echo "build-thirdparty-ci: expected executable missing: $src" >&2
    exit 1
  fi
  install -D -m 0755 "$src" "$dst"
}

copy_required_payload_file() {
  local src=$1
  local dst=$2
  if [ ! -f "$src" ]; then
    echo "build-thirdparty-ci: expected payload input missing: $src" >&2
    exit 1
  fi
  install -D -m 0644 "$src" "$dst"
}

busybox_root=$src_dir/../build/rootfs
tcc_build=$src_dir/../build/tcc

if [ "$skip_busybox" = "0" ]; then
  copy_executable "$busybox_root/bin/busybox" "$payload_dir/bin/busybox"
fi
copy_executable "$src_dir/python/ports/unix/build-minimal/micropython" "$payload_dir/bin/micropython"
copy_executable "$tcc_build/cc" "$payload_dir/bin/tcc"
copy_executable "$work_top/build/thirdparty/make/make" "$payload_dir/bin/pdpmake"

if [ "$skip_busybox" = "0" ]; then
  copy_dir "$busybox_root" "$payload_dir/busybox-root"
fi
copy_dir "$src_dir/python-examples" "$payload_dir/python-examples"
copy_dir "$busybox_root/lib/tcc/include" "$payload_dir/tcc-sysroot/include"
copy_dir "$busybox_root/include" "$payload_dir/tcc-sysroot/include"

copy_required_payload_file "$tcc_build/libtcc1.a" "$payload_dir/tcc-sysroot/lib/libtcc1.a"
copy_required_payload_file "$tcc_build/runmain.o" "$payload_dir/tcc-sysroot/lib/runmain.o"
copy_required_payload_file "$repo_root/build/libc/crt.S.o" "$payload_dir/tcc-sysroot/lib/crt1.o"
copy_required_payload_file "$repo_root/build/libc.a" "$payload_dir/tcc-sysroot/lib/libc.a"

touch "$payload_dir/tcc-sysroot/lib/crti.o" "$payload_dir/tcc-sysroot/lib/crtn.o"

required_paths=(
  "$payload_dir/bin/micropython"
  "$payload_dir/bin/tcc"
  "$payload_dir/bin/pdpmake"
  "$payload_dir/tcc-sysroot/include/stdio.h"
  "$payload_dir/tcc-sysroot/lib/crt1.o"
  "$payload_dir/tcc-sysroot/lib/libc.a"
  "$payload_dir/tcc-sysroot/lib/libtcc1.a"
)

if [ "$skip_busybox" = "0" ]; then
  required_paths+=(
    "$payload_dir/bin/busybox"
    "$payload_dir/busybox-root/bin/cat"
    "$payload_dir/busybox-root/bin/cp"
    "$payload_dir/busybox-root/bin/echo"
    "$payload_dir/busybox-root/bin/ls"
    "$payload_dir/busybox-root/bin/mkdir"
    "$payload_dir/busybox-root/bin/mv"
    "$payload_dir/busybox-root/bin/rm"
    "$payload_dir/busybox-root/bin/rmdir"
  )
fi

for path in "${required_paths[@]}"; do
  if [ ! -e "$path" ]; then
    echo "build-thirdparty-ci: payload validation failed: $path" >&2
    exit 1
  fi
done

printf 'THIRDPARTY_CI_SKIP_BUSYBOX=%s\n' "$skip_busybox" >"$payload_dir/.complete"
cp "$payload_dir/.complete" "$payload_dir/.complete.skip-busybox-$skip_busybox"
