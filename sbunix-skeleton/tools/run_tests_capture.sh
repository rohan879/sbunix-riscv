#!/bin/bash
# Preserve per-boot raw logs because tools/run_tests.sh deletes its private
# capture file on exit, which is not sufficient for the final CR08 evidence.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TIMEOUT=${TIMEOUT:-7200}
ARTIFACT_DIR=${SBUNIX_RUN_TESTS_CAPTURE_DIR:-/home/adebayo/test-findings/code-review-08-artifacts/final}
BOOT1_LOG="$ARTIFACT_DIR/qemu-boot1.raw.log"
BOOT2_LOG="$ARTIFACT_DIR/qemu-boot2.raw.log"
COMBINED_LOG="$ARTIFACT_DIR/qemu-combined.raw.log"
STATUS_FILE="$ARTIFACT_DIR/host-run-tests.status"
QEMU_PID=""
QEMU_FIFO_DIR=""
QEMU_FIFO=""
QEMU_STDIN_FD=3
BOOT_DEADLINE=0

cleanup() {
    status=$?
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
    mkdir -p "$ARTIFACT_DIR"
    printf '%s\n' "$status" > "$STATUS_FILE"
}
trap cleanup EXIT

mkdir -p "$ARTIFACT_DIR"
: > "$BOOT1_LOG"
: > "$BOOT2_LOG"
: > "$COMBINED_LOG"
QEMU_FIFO_DIR="$(mktemp -d /tmp/sbunix-qemu-stdin.XXXXXX)"
QEMU_FIFO="$QEMU_FIFO_DIR/stdin.fifo"
mkfifo "$QEMU_FIFO"
# Keep the FIFO open for the lifetime of the wrapper so QEMU does not see
# EOF between injected guest commands.
exec {QEMU_STDIN_FD}<>"$QEMU_FIFO"

cd "$PROJECT_DIR"

# Set up cross-compiler PATH.
source tools/sourceme.sh

echo "==> Building kernel..."
MAKEFLAGS= make clean && mkdir -p build/kernel build/libc build/rootfs/bin && MAKEFLAGS= make -j1 BUILD_TESTS=1
if [ $? -ne 0 ]; then
    echo "FATAL: Kernel build failed"
    exit 1
fi

echo "==> Building disk image..."
make build/disk.img
if [ $? -ne 0 ]; then
    echo "FATAL: Disk image build failed"
    exit 1
fi

echo "==> Building test disk image..."
make BUILD_TESTS=1 build/test-disk.img
if [ $? -ne 0 ]; then
    echo "FATAL: Test disk image build failed"
    exit 1
fi

run_qemu_boot() {
    local label="$1"
    local inject_full_suite="$2"
    local boot_log="$3"

    : > "$boot_log"

    echo "==> Launching QEMU ($label, timeout: ${TIMEOUT}s)..."
    qemu-system-riscv64 -machine virt -bios default -kernel build/kernel.elf \
        -drive file=build/disk.img,format=raw,if=none,id=hd0 \
        -device virtio-blk-pci-non-transitional,drive=hd0 \
        -drive file=build/test-disk.img,format=raw,if=none,id=hd1 \
        -device virtio-blk-pci-non-transitional,drive=hd1 \
        -device virtio-gpu-pci \
        -device virtio-net-pci-non-transitional,netdev=net0 \
        -netdev user,id=net0 \
        -nographic \
        <&"$QEMU_STDIN_FD" \
        >> "$boot_log" 2>&1 &
    QEMU_PID=$!
    BOOT_DEADLINE=$((SECONDS + TIMEOUT))

    wait_for_prompt_since() {
        local start_size="$1"
        while kill -0 "$QEMU_PID" 2>/dev/null; do
            sleep 5
            if tail -c +"$((start_size + 1))" "$boot_log" 2>/dev/null | grep -q 'sbush\$'; then
                return 0
            fi
            if [ "$SECONDS" -ge "$BOOT_DEADLINE" ]; then
                echo "==> $label: timeout reached (${TIMEOUT}s), stopping QEMU..."
                kill "$QEMU_PID" 2>/dev/null || true
                wait "$QEMU_PID" 2>/dev/null || true
                QEMU_PID=""
                return 1
            fi
        done
        return 1
    }

    if [ "$inject_full_suite" = "yes" ]; then
        local start_size=0
        if ! wait_for_prompt_since "$start_size"; then
            cat "$boot_log" >> "$COMBINED_LOG"
            return
        fi

        echo "==> $label: initial prompt reached"

        start_size=$(wc -c < "$boot_log" 2>/dev/null || echo 0)
        echo "==> $label: running Sortix suite..."
        printf 'run-os-test\n' >&"$QEMU_STDIN_FD"
        if ! wait_for_prompt_since "$start_size"; then
            cat "$boot_log" >> "$COMBINED_LOG"
            return
        fi

        start_size=$(wc -c < "$boot_log" 2>/dev/null || echo 0)
        echo "==> $label: running Open POSIX suite..."
        printf 'run-open-posix-test\n' >&"$QEMU_STDIN_FD"
        if ! wait_for_prompt_since "$start_size"; then
            cat "$boot_log" >> "$COMBINED_LOG"
            return
        fi

        echo "==> $label: running persist write phase..."
        printf '/test/bin/persisttest\n' >&"$QEMU_STDIN_FD"
        while kill -0 "$QEMU_PID" 2>/dev/null; do
            sleep 5
            if [ "$SECONDS" -ge "$BOOT_DEADLINE" ]; then
                echo "==> $label: timeout reached (${TIMEOUT}s), stopping QEMU..."
                kill "$QEMU_PID" 2>/dev/null || true
                wait "$QEMU_PID" 2>/dev/null || true
                QEMU_PID=""
                break
            fi
        done
    else
        while kill -0 "$QEMU_PID" 2>/dev/null; do
            sleep 5
            if [ "$SECONDS" -ge "$BOOT_DEADLINE" ]; then
                echo "==> $label: timeout reached (${TIMEOUT}s), stopping QEMU..."
                kill "$QEMU_PID" 2>/dev/null || true
                wait "$QEMU_PID" 2>/dev/null || true
                QEMU_PID=""
                break
            fi
        done
    fi

    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=""
    cat "$boot_log" >> "$COMBINED_LOG"
}

# Boot 1: rc runs the in-tree suite, then the wrapper drives the full Sortix
# and open-posix suites plus the persist write phase from the interactive shell.
run_qemu_boot "Boot 1: tests + persist write" "yes" "$BOOT1_LOG"

# Boot 2: persist verify only (rc detects sentinel, runs persisttest, poweroffs).
run_qemu_boot "Boot 2: persist verify" "no" "$BOOT2_LOG"

echo "==> QEMU exited, parsing results..."
echo ""

pass_count=0
fail_count=0
failed_tests=()

while IFS= read -r line; do
    line="${line//$'\r'/}"
    case "$line" in
        *[^$'\t'$'\n'\ -~]*) continue ;;
    esac
    if [[ "$line" =~ \[PASS\]\ ([a-zA-Z_].+) ]]; then
        pass_count=$((pass_count + 1))
    elif [[ "$line" =~ \[FAIL\]\ ([a-zA-Z_].+) ]]; then
        fail_count=$((fail_count + 1))
        test_name="${BASH_REMATCH[1]}"
        test_name="$(echo "$test_name" | sed 's/^[[:space:]]*//')"
        failed_tests+=("$test_name")
    fi
done < "$COMBINED_LOG"

total=$((pass_count + fail_count))

echo "==============================="
echo "  SBUnix Test Results"
echo "==============================="
echo "  Total:  $total"
echo "  Passed: $pass_count"
echo "  Failed: $fail_count"
echo "==============================="

if [ "$fail_count" -gt 0 ]; then
    echo ""
    echo "Failed tests:"
    for name in "${failed_tests[@]}"; do
        echo "  - $name"
    done
    echo ""
    exit 1
fi

if [ "$total" -eq 0 ]; then
    echo ""
    echo "FATAL: No test results found in output."
    echo "Ensure the OS boots and runs test binaries that emit [PASS]/[FAIL]."
    echo ""
    exit 1
fi

exit 0
