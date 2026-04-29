#!/bin/bash
# Targeted SBUnix QEMU runner for focused VM/syscall regression gates.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_DIR="$(git -C "$PROJECT_DIR" rev-parse --show-toplevel 2>/dev/null || echo "$PROJECT_DIR")"
TIMEOUT=${TIMEOUT:-180}
# QEMU_MEM must remain 128M for remote OS/build checks.
QEMU_MEM=${QEMU_MEM:-128M}
MAKE_JOBS=${MAKE_JOBS:-1}
TEST_DISK_IMG=${TEST_DISK_IMG:-build/test-disk-core.img}
TEST_DISK_TARGET=${TEST_DISK_TARGET:-$TEST_DISK_IMG}
ARTIFACT_DIR=${SBUNIX_TARGETED_ARTIFACT_DIR:-/home/adebayo/test-findings/test-findings-23-artifacts/targeted}
RAW_LOG="$ARTIFACT_DIR/targeted-qemu.raw.log"
SUMMARY_LOG="$ARTIFACT_DIR/targeted-qemu-summary.txt"
QEMU_PID=""
QEMU_FIFO_DIR=""
QEMU_FIFO=""
QEMU_STDIN_FD=3
FAILURES=0

DEFAULT_COMMANDS=(
    # QEMU_MEM must remain 128M; keep post-fork dirty allocation pressure
    # within the available guest RAM after kernel and test image overhead.
    "/test/bin/forkmemdiag postfork 30 60"
    "/test/bin/forkmemdiag cow 30"
    "/test/bin/forkreuse"
    "/test/bin/lazyreserve"
    "/test/bin/mremaptest"
    "/test/bin/mprotecttest"
    "/test/bin/lazytest"
    "/test/bin/mmaptest"
    "/test/bin/copyin_lazy"
    "/test/bin/copyout_lazy"
    "/test/bin/copyinstr_lazy"
    "/test/bin/membugstest"
    # TEST: Low-memory recovery after normal exit and SIGKILL must remain
    # in the default smoke set because these paths recycle physical pages.
    "/test/bin/mempressure_recovery"
)

cleanup() {
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if [ -n "$QEMU_FIFO" ]; then
        exec {QEMU_STDIN_FD}>&- 2>/dev/null || true
        rm -f "$QEMU_FIFO"
    fi
    if [ -n "$QEMU_FIFO_DIR" ]; then
        rmdir "$QEMU_FIFO_DIR" 2>/dev/null || true
    fi
}
trap cleanup EXIT

sanitize_id() {
    printf '%s' "$1" |
        tr -cs '[:alnum:]' '_' |
        sed 's/^_//; s/_$//' |
        cut -c 1-64
}

wait_for_pattern_since() {
    local start_size="$1"
    local pattern="$2"
    local deadline=$((SECONDS + TIMEOUT))

    while kill -0 "$QEMU_PID" 2>/dev/null; do
        sleep 1
        if tail -c +"$((start_size + 1))" "$RAW_LOG" 2>/dev/null | grep -F -q "$pattern"; then
            return 0
        fi
        if [ "$SECONDS" -ge "$deadline" ]; then
            echo "timeout waiting for $pattern" | tee -a "$SUMMARY_LOG"
            kill "$QEMU_PID" 2>/dev/null || true
            wait "$QEMU_PID" 2>/dev/null || true
            QEMU_PID=""
            return 1
        fi
    done
    return 1
}

wait_for_status_since() {
    local start_size="$1"
    local id="$2"
    local deadline=$((SECONDS + TIMEOUT))

    while kill -0 "$QEMU_PID" 2>/dev/null; do
        sleep 1
        if tail -c +"$((start_size + 1))" "$RAW_LOG" 2>/dev/null |
            tr -d '\r' |
            grep -E -q "__STATUS:${id}:[0-9]"; then
            return 0
        fi
        if [ "$SECONDS" -ge "$deadline" ]; then
            echo "timeout waiting for status $id" | tee -a "$SUMMARY_LOG"
            kill "$QEMU_PID" 2>/dev/null || true
            wait "$QEMU_PID" 2>/dev/null || true
            QEMU_PID=""
            return 1
        fi
    done
    return 1
}

run_guest_command() {
    local cmd="$1"
    local id="$2"
    local start_size
    local segment
    local status

    start_size=$(wc -c < "$RAW_LOG" 2>/dev/null || echo 0)
    printf '\necho __BEGIN:%s\n%s\necho __STATUS:%s:$?\necho __END:%s\n' \
        "$id" "$cmd" "$id" "$id" >&"$QEMU_STDIN_FD"

    if ! wait_for_status_since "$start_size" "$id"; then
        echo "[FAIL] $cmd (missing status marker)" | tee -a "$SUMMARY_LOG"
        FAILURES=$((FAILURES + 1))
        return
    fi

    segment="$(tail -c +"$((start_size + 1))" "$RAW_LOG" 2>/dev/null | tr -d '\r')"
    status="$(printf '%s\n' "$segment" |
        sed -n "s/.*__STATUS:$id:\([0-9][0-9]*\).*/\1/p" |
        tail -n 1)"

    if [ -z "$status" ]; then
        echo "[FAIL] $cmd (missing status marker)" | tee -a "$SUMMARY_LOG"
        FAILURES=$((FAILURES + 1))
        return
    fi

    if [ "$status" != "0" ]; then
        echo "[FAIL] $cmd (status $status)" | tee -a "$SUMMARY_LOG"
        FAILURES=$((FAILURES + 1))
        return
    fi

    if ! wait_for_pattern_since "$start_size" 'sbush$'; then
        echo "[FAIL] $cmd (prompt not returned)" | tee -a "$SUMMARY_LOG"
        FAILURES=$((FAILURES + 1))
        return
    fi

    if printf '%s\n' "$segment" |
        grep -E -q '\[FAIL\]|panic|Page fault|pte-update violation|kernel PTE repair|WARNING'; then
        # BF: Targeted gates must fail on kernel diagnostics even when the
        # shell command itself exits zero.
        echo "[FAIL] $cmd (failure token in log)" | tee -a "$SUMMARY_LOG"
        FAILURES=$((FAILURES + 1))
        return
    fi

    echo "[PASS] $cmd" | tee -a "$SUMMARY_LOG"
}

mkdir -p "$ARTIFACT_DIR"
: > "$RAW_LOG"
: > "$SUMMARY_LOG"

cd "$PROJECT_DIR"
source tools/sourceme.sh

IMPLEMENTATION_SHA="$(git -C "$REPO_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
echo "implementation_sha=$IMPLEMENTATION_SHA" | tee -a "$SUMMARY_LOG"
echo "test_disk_img=$TEST_DISK_IMG" | tee -a "$SUMMARY_LOG"
if [ "${ALLOW_DIRTY_VALIDATION:-0}" != "1" ] &&
   [ -n "$(git -C "$REPO_DIR" status --porcelain -- sbunix-skeleton 2>/dev/null)" ]; then
    echo "FATAL: sbunix-skeleton has uncommitted changes; set ALLOW_DIRTY_VALIDATION=1 for development runs." | tee -a "$SUMMARY_LOG"
    exit 1
fi

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    # BF: Header-layout changes otherwise leave stale kernel objects because
    # the simple Makefile does not maintain full generated header deps.
    if [ "${SKIP_CLEAN:-0}" != "1" ]; then
        MAKEFLAGS= make clean
    fi
    MAKEFLAGS= make -j"$MAKE_JOBS" build/kernel.elf build/disk.img "$TEST_DISK_TARGET"
fi

QEMU_FIFO_DIR="$(mktemp -d /tmp/sbunix-targeted-stdin.XXXXXX)"
QEMU_FIFO="$QEMU_FIFO_DIR/stdin.fifo"
mkfifo "$QEMU_FIFO"
exec {QEMU_STDIN_FD}<>"$QEMU_FIFO"

qemu-system-riscv64 -machine virt -m "$QEMU_MEM" -bios default -kernel build/kernel.elf \
    -drive file=build/disk.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-pci-non-transitional,drive=hd0 \
    -drive file="$TEST_DISK_IMG",format=raw,if=none,id=hd1 \
    -device virtio-blk-pci-non-transitional,drive=hd1 \
    -device virtio-gpu-pci \
    -device virtio-net-pci-non-transitional,netdev=net0 \
    -netdev user,id=net0 \
    -nographic \
    <&"$QEMU_STDIN_FD" \
    >> "$RAW_LOG" 2>&1 &
QEMU_PID=$!

if ! wait_for_pattern_since 0 'sbush$'; then
    echo "FATAL: shell prompt not reached" | tee -a "$SUMMARY_LOG"
    exit 1
fi

if [ "$#" -gt 0 ]; then
    COMMANDS=("$@")
else
    COMMANDS=("${DEFAULT_COMMANDS[@]}")
fi

for cmd in "${COMMANDS[@]}"; do
    id="$(sanitize_id "$cmd")"
    if [ -z "$id" ]; then
        id="cmd"
    fi
    run_guest_command "$cmd" "$id"
done

printf 'poweroff\n' >&"$QEMU_STDIN_FD" || true
sleep 2

if [ "$FAILURES" -gt 0 ]; then
    echo "targeted_qemu_failures=$FAILURES" | tee -a "$SUMMARY_LOG"
    exit 1
fi

echo "targeted_qemu_failures=0" | tee -a "$SUMMARY_LOG"
exit 0
