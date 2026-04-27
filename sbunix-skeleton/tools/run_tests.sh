#!/bin/bash
# SBUnix host-side test runner
# Builds the kernel, launches QEMU with a timeout, and parses test results.
# Supports two-boot persistence testing: Boot 1 runs all tests + persist write
# (which calls poweroff), Boot 2 runs persist verify only.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_DIR="$(git -C "$PROJECT_DIR" rev-parse --show-toplevel 2>/dev/null || echo "$PROJECT_DIR")"
TIMEOUT=${TIMEOUT:-900}
# BF: Match the kernel PHYSTOP default; higher QEMU_MEM can be set explicitly.
QEMU_MEM=${QEMU_MEM:-128M}
OUTPUT_FILE="$(mktemp /tmp/sbunix-test-output.XXXXXX)"
QEMU_PID=""
QEMU_FIFO_DIR=""
QEMU_FIFO=""
QEMU_STDIN_FD=3
BOOT_FAILED=0

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
    rm -f "$OUTPUT_FILE"
}
trap cleanup EXIT

cd "$PROJECT_DIR"

# Set up cross-compiler PATH
source tools/sourceme.sh

IMPLEMENTATION_SHA="$(git -C "$REPO_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
echo "==> Validation implementation SHA: $IMPLEMENTATION_SHA"
if [ "${ALLOW_DIRTY_VALIDATION:-0}" != "1" ] &&
   [ -n "$(git -C "$REPO_DIR" status --porcelain -- sbunix-skeleton 2>/dev/null)" ]; then
    # BF: Validation must be tied to a committed implementation tree, not
    # whichever uncommitted edits happen to be present during a run.
    echo "FATAL: sbunix-skeleton has uncommitted changes; set ALLOW_DIRTY_VALIDATION=1 for development runs."
    exit 1
fi

# Build the kernel
echo "==> Building kernel..."
MAKEFLAGS= make clean && mkdir -p build/kernel build/libc build/rootfs/bin && MAKEFLAGS= make -j1 BUILD_TESTS=1
if [ $? -ne 0 ]; then
    echo "FATAL: Kernel build failed"
    exit 1
fi

# Build the disk image
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

QEMU_FIFO_DIR="$(mktemp -d /tmp/sbunix-qemu-stdin.XXXXXX)"
QEMU_FIFO="$QEMU_FIFO_DIR/stdin.fifo"
mkfifo "$QEMU_FIFO"
exec {QEMU_STDIN_FD}<>"$QEMU_FIFO"

# --- Helper: run one QEMU boot, appending output to OUTPUT_FILE ---
run_qemu_boot() {
    local label="$1"
    local check_prompt="$2"  # "yes" or "no"
    local start_size
    local persist_sent=0
    start_size=$(wc -c < "$OUTPUT_FILE" 2>/dev/null || echo 0)

    echo "==> Launching QEMU ($label, timeout: ${TIMEOUT}s)..."
    qemu-system-riscv64 -machine virt -m "$QEMU_MEM" -bios default -kernel build/kernel.elf \
        -drive file=build/disk.img,format=raw,if=none,id=hd0 \
        -device virtio-blk-pci-non-transitional,drive=hd0 \
        -drive file=build/test-disk.img,format=raw,if=none,id=hd1 \
        -device virtio-blk-pci-non-transitional,drive=hd1 \
        -device virtio-gpu-pci \
        -device virtio-net-pci-non-transitional,netdev=net0 \
        -netdev user,id=net0 \
        -nographic \
        <&"$QEMU_STDIN_FD" \
        >> "$OUTPUT_FILE" 2>&1 &
    QEMU_PID=$!

    ELAPSED=0
    while kill -0 "$QEMU_PID" 2>/dev/null; do
        sleep 5
        ELAPSED=$((ELAPSED + 5))
        if [ "$check_prompt" = "yes" ]; then
            # Only check NEW output for the shell prompt (avoid matching Boot 1 output)
            if [ "$persist_sent" -eq 0 ] &&
               tail -c +"$((start_size + 1))" "$OUTPUT_FILE" 2>/dev/null | grep -q 'sbush\$'; then
                echo "==> $label: tests completed, running persist write..."
                printf '\n/test/bin/persisttest\n' >&"$QEMU_STDIN_FD"
                persist_sent=1
            fi
        fi
        if [ "$ELAPSED" -ge "$TIMEOUT" ]; then
            echo "==> $label: timeout reached (${TIMEOUT}s), stopping QEMU..." | tee -a "$OUTPUT_FILE"
            BOOT_FAILED=1
            kill "$QEMU_PID" 2>/dev/null || true
            wait "$QEMU_PID" 2>/dev/null || true
            QEMU_PID=""
            break
        fi
    done
    if [ "$check_prompt" = "yes" ] && [ "$persist_sent" -eq 0 ]; then
        echo "==> $label: shell prompt was not reached before QEMU exited." | tee -a "$OUTPUT_FILE"
        BOOT_FAILED=1
    fi
    # QEMU may have exited on its own (poweroff)
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=""
}

# Boot 1: run all tests; persisttest write phase calls poweroff at end
run_qemu_boot "Boot 1: tests + persist write" "yes"

# Boot 2: persist verify only (rc detects sentinel, runs persisttest, poweroffs)
run_qemu_boot "Boot 2: persist verify" "no"

echo "==> QEMU exited, parsing results..."
echo ""

# Parse [PASS] and [FAIL] lines from combined output of both boots
pass_count=0
fail_count=0
failed_tests=()

while IFS= read -r line; do
    # Strip carriage returns
    line="${line//$'\r'/}"
    # Skip lines containing non-printable/non-ASCII characters (garbled UART)
    case "$line" in
        *[^$'\t'$'\n'\ -~]*) continue ;;
    esac
    # Require test name to start with a word character (rejects garbled fragments)
    if [[ "$line" =~ \[PASS\]\ ([a-zA-Z_].+) ]]; then
        pass_count=$((pass_count + 1))
    elif [[ "$line" =~ \[FAIL\]\ ([a-zA-Z_].+) ]]; then
        fail_count=$((fail_count + 1))
        test_name="${BASH_REMATCH[1]}"
        test_name="$(echo "$test_name" | sed 's/^[[:space:]]*//')"
        failed_tests+=("$test_name")
    fi
done < "$OUTPUT_FILE"

total=$((pass_count + fail_count))

require_output() {
    local pattern="$1"
    local label="$2"

    if ! grep -q "$pattern" "$OUTPUT_FILE"; then
        # BF: A boot can time out after partial PASS output.  Require
        # end-of-phase markers so truncated logs cannot validate cleanly.
        echo "FATAL: missing required output marker: $label"
        BOOT_FAILED=1
    fi
}

require_output "=== Summary ===" "guest-run-tests summary"
require_output "persist: write phase complete" "persistence write completion"
require_output "persist: verify phase complete" "persistence verify completion"

# Print summary
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

if [ "$BOOT_FAILED" -ne 0 ]; then
    echo ""
    echo "FATAL: one or more boot phases did not complete cleanly."
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
