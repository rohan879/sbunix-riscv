#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_DIR="$(git -C "$PROJECT_DIR" rev-parse --show-toplevel 2>/dev/null || echo "$PROJECT_DIR")"
ARTIFACT_DIR=${SBUNIX_EVAL29_ARTIFACT_DIR:-/home/adebayo/test-findings/test-findings-29-artifacts/final}
MAKE_JOBS=${MAKE_JOBS:-4}
QEMU_MEM=${QEMU_MEM:-128M}
TIMEOUT=${TIMEOUT:-28800}

LOG_DIR="$ARTIFACT_DIR/logs"
ABI_DIR="$ARTIFACT_DIR/abi"
SUMMARY="$ARTIFACT_DIR/eval29-summary.txt"

mkdir -p "$LOG_DIR" "$ABI_DIR"
: > "$SUMMARY"

log() {
  printf '%s\n' "$*" | tee -a "$SUMMARY"
}

run_logged() {
  local name=$1
  shift
  log "==> $name"
  "$@" >"$LOG_DIR/$name.log" 2>&1
}

cd "$PROJECT_DIR"
# shellcheck disable=SC1091
source tools/sourceme.sh

GCC_INC="$(riscv64-unknown-elf-gcc -print-file-name=include)"
CFLAGS_ABI=(-Wall -Werror -ffreestanding -nostdlib -nostdinc
  -mcmodel=medany -march=rv64imac_zicsr_zifencei -mabi=lp64
  -isystem "$PROJECT_DIR/thirdparty/include"
  -isystem "$PROJECT_DIR/libc/include"
  -isystem "$GCC_INC")

log "eval29 validation"
log "implementation_sha=$(git -C "$REPO_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
log "branch=$(git -C "$REPO_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
log "origin_main_sha=$(git -C "$REPO_DIR" rev-parse origin/main 2>/dev/null || echo unavailable)"
log "qemu_mem=$QEMU_MEM"
log "make_jobs=$MAKE_JOBS"
log "timeout=$TIMEOUT"
git -C "$REPO_DIR" status --short >"$ARTIFACT_DIR/git-status.txt" 2>/dev/null || true

write_abi_canaries() {
  cat >"$ABI_DIR/include_order_sys_time_first.c" <<'CANARY'
#include <sys/time.h>
#include <time.h>
#include <sys/select.h>
int gettimeofday(struct timeval *restrict, struct timezone *restrict);
int settimeofday(const struct timeval *, const struct timezone *);
int utimes(const char *, const struct timeval[2]);
int main(void) {
  struct timeval tv = {0};
  struct timezone tz = {0};
  struct timespec ts = {0};
  fd_set rfds;
  FD_ZERO(&rfds);
  return (int)(tv.tv_sec + tz.tz_dsttime + ts.tv_nsec);
}
CANARY

  cat >"$ABI_DIR/include_order_time_first.c" <<'CANARY'
#include <time.h>
#include <sys/time.h>
#include <sys/select.h>
int clock_gettime(clockid_t, struct timespec *);
int clock_nanosleep(clockid_t, int, const struct timespec *, struct timespec *);
int main(void) {
  struct timeval tv = {0};
  struct timespec ts = {0};
  return (int)(tv.tv_usec + ts.tv_sec);
}
CANARY

  cat >"$ABI_DIR/select_time_canary.c" <<'CANARY'
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
int select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int pselect(int, fd_set *, fd_set *, fd_set *, const struct timespec *,
            const sigset_t *);
int main(void) {
  fd_set set;
  struct timeval tv = {0};
  struct timespec ts = {0};
  sigset_t mask = 0;
  FD_ZERO(&set);
  return select(0, &set, 0, 0, &tv) + pselect(0, 0, 0, 0, &ts, &mask);
}
CANARY

  cat >"$ABI_DIR/thirdparty_overlay_canary.c" <<'CANARY'
#include <sys/time.h>
#include <time.h>
int main(void) {
  struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
  struct timezone tz = {0};
  struct timespec ts = {0};
  (void)gettimeofday(&tv, &tz);
  (void)settimeofday(&tv, &tz);
  (void)clock_gettime(CLOCK_REALTIME, &ts);
  (void)clock_nanosleep(CLOCK_REALTIME, 0, &ts, 0);
  return 0;
}
CANARY
}

compile_abi_canaries() {
  write_abi_canaries
  for src in include_order_sys_time_first include_order_time_first select_time_canary; do
    riscv64-unknown-elf-gcc "${CFLAGS_ABI[@]}" \
      -c "$ABI_DIR/$src.c" -o "$ABI_DIR/$src.o"
  done
}

check_strong_symbols() {
  local nm_out="$ABI_DIR/libc-symbols.txt"
  riscv64-unknown-elf-nm build/libc.a >"$nm_out"
  grep -Eq '(^|[[:space:]])T[[:space:]]+settimeofday$' "$nm_out"
  grep -Eq '(^|[[:space:]])T[[:space:]]+gettimeofday$' "$nm_out"
  grep -Eq '(^|[[:space:]])T[[:space:]]+clock_gettime$' "$nm_out"
  grep -Eq '(^|[[:space:]])T[[:space:]]+clock_nanosleep$' "$nm_out"
}

link_overlay_canary() {
  MAKEFLAGS= make -C thirdparty \
    "$PROJECT_DIR/build/thirdparty/dummy.o" \
    "$PROJECT_DIR/build/thirdparty/compat.o" \
    "$PROJECT_DIR/build/thirdparty/setjmp.o"
  riscv64-unknown-elf-gcc "${CFLAGS_ABI[@]}" \
    -c "$ABI_DIR/thirdparty_overlay_canary.c" \
    -o "$ABI_DIR/thirdparty_overlay_canary.o"
  riscv64-unknown-elf-gcc -nostdlib build/libc/crt.S.o \
    "$ABI_DIR/thirdparty_overlay_canary.o" \
    -Wl,--whole-archive build/libc.a -Wl,--no-whole-archive \
    build/thirdparty/dummy.o build/thirdparty/compat.o build/thirdparty/setjmp.o \
    -Wl,--allow-multiple-definition -Wl,--gc-sections \
    -o "$ABI_DIR/thirdparty_overlay_canary"
}

sortix_target_for_source() {
  local rel=$1
  printf '%s/build/testfs/bin/os-test/%s' "$PROJECT_DIR" "${rel%.c}"
}

open_posix_target_for_source() {
  local rel=$1
  printf '%s/build/testfs/bin/open-posix-test/%s' "$PROJECT_DIR" "${rel%.c}"
}

build_sortix_source() {
  local rel=$1
  local src="tests/thirdparty/sortix/os-test/$rel"
  local target
  target="$(sortix_target_for_source "$rel")"
  [ -f "$src" ] || { log "missing Sortix source: $src"; return 1; }
  MAKEFLAGS= make -f tools/os-test.mk SBUNIX_ROOT="$PROJECT_DIR" "$target"
  [ ! -e "$target.failed" ] || { log "Sortix focused compile failed: $rel"; return 1; }
}

build_open_posix_source() {
  local rel=$1
  local src="tests/thirdparty/open-posix-test-suite/posixtest/$rel"
  local target
  target="$(open_posix_target_for_source "$rel")"
  [ -f "$src" ] || { log "missing Open POSIX source: $src"; return 1; }
  MAKEFLAGS= make -f tools/open-posix-test.mk SBUNIX_ROOT="$PROJECT_DIR" "$target"
  [ ! -e "$target.failed" ] || { log "Open POSIX focused compile failed: $rel"; return 1; }
}

focused_sortix_gates() {
  local rel
  while IFS= read -r rel; do
    build_sortix_source "$rel"
  done < <(find tests/thirdparty/sortix/os-test/include/sys_time \
              -maxdepth 1 -type f -name '*.c' -printf 'include/sys_time/%f\n' | sort)
  while IFS= read -r rel; do
    build_sortix_source "$rel"
  done < <(find tests/thirdparty/sortix/os-test/include/sys_select \
              -maxdepth 1 -type f -name '*.c' -printf 'include/sys_select/%f\n' | sort)
  build_sortix_source "include/time/clock_gettime.c"
}

focused_open_posix_gates() {
  build_open_posix_source "conformance/interfaces/clock_gettime/1-2.c"
  build_open_posix_source "conformance/interfaces/clock_gettime/2-1.c"
}

build_micropython_if_available() {
  if [ -d thirdparty/python/ports/unix ]; then
    MAKEFLAGS= make -C thirdparty python
    [ -x thirdparty/python/ports/unix/build-minimal/micropython ]
    if git -C thirdparty/python rev-parse --is-inside-work-tree >/dev/null 2>&1; then
      # OPS: The local python target applies python-sbunix.patch before
      # building.  Restore the source file after the binary exists so the
      # outer validation can still enforce a clean implementation tree.
      git -C thirdparty/python checkout -- ports/unix/mpconfigport.h
    fi
  else
    log "MicroPython source directory missing; source build skipped"
  fi
}

check_micropython_staged() {
  if [ -x thirdparty/python/ports/unix/build-minimal/micropython ]; then
    [ -x build/testfs/bin/rootfs/python ]
    [ -x build/testfs/bin/rootfs/micropython ]
    [ -L build/tarfs-rootfs/bin/python ] || [ -x build/tarfs-rootfs/bin/python ]
    [ -L build/tarfs-rootfs/bin/micropython ] || [ -x build/tarfs-rootfs/bin/micropython ]
  fi
}

parse_suite_metrics() {
  local boot_log="$ARTIFACT_DIR/full/qemu-boot1.raw.log"
  local norm="$ARTIFACT_DIR/full/qemu-boot1.normalized.log"
  tr -d '\r' < "$boot_log" > "$norm"

  local sortix_compiled sortix_passed open_compiled open_passed
  sortix_compiled="$(awk '/=== Sortix os-test Suite ===/{seen=1} seen && /Compiled:/{print $2; exit}' "$norm")"
  sortix_passed="$(awk '/=== Sortix os-test Suite ===/{seen=1} seen && /  Passed:/{print $2; exit}' "$norm")"
  open_compiled="$(awk '/=== Open POSIX Test Suite ===/{seen=1} seen && /Compiled:/{print $2; exit}' "$norm")"
  open_passed="$(awk '/=== Open POSIX Test Suite ===/{seen=1} seen && /  Passed:/{print $2; exit}' "$norm")"

  log "sortix_compiled=${sortix_compiled:-missing}"
  log "sortix_passed=${sortix_passed:-missing}"
  log "open_posix_compiled=${open_compiled:-missing}"
  log "open_posix_passed=${open_passed:-missing}"

  [ "${sortix_compiled:-0}" -ge 4709 ]
  [ "${sortix_passed:-0}" -ge 4508 ]
  [ "${open_compiled:-0}" -ge 1030 ]
  [ "${open_passed:-0}" -ge 915 ]
}

run_logged abi-canaries compile_abi_canaries
run_logged libc-build env MAKEFLAGS= make -j"$MAKE_JOBS" build/libc.a build/libc/crt.S.o
run_logged strong-symbols check_strong_symbols
run_logged thirdparty-overlay link_overlay_canary
run_logged focused-sortix focused_sortix_gates
run_logged focused-open-posix focused_open_posix_gates
run_logged micropython-build build_micropython_if_available

log "==> full-qemu-validation"
SBUNIX_RUN_TESTS_CAPTURE_DIR="$ARTIFACT_DIR/full" \
QEMU_MEM="$QEMU_MEM" MAKE_JOBS="$MAKE_JOBS" TIMEOUT="$TIMEOUT" \
  tools/run_tests_capture.sh >"$LOG_DIR/full-qemu-validation.log" 2>&1

run_logged micropython-staged check_micropython_staged
run_logged suite-thresholds parse_suite_metrics

log "eval29 validation passed"
