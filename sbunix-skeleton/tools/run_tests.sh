#!/bin/bash
# SBUnix host-side test runner
# Builds the kernel, launches QEMU with a timeout, and parses test results.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TIMEOUT=600
OUTPUT_FILE="$(mktemp /tmp/sbunix-test-output.XXXXXX)"
QEMU_PID=""

cleanup() {
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -f "$OUTPUT_FILE"
}
trap cleanup EXIT

cd "$PROJECT_DIR"

# Set up cross-compiler PATH
source tools/sourceme.sh

# Build the kernel
echo "==> Building kernel..."
make clean && make
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

# Launch QEMU in background, capturing serial output
echo "==> Launching QEMU (timeout: ${TIMEOUT}s)..."
qemu-system-riscv64 -machine virt -bios default -kernel build/kernel.elf \
    -drive file=build/disk.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-pci-non-transitional,drive=hd0 \
    -device virtio-gpu-pci \
    -device virtio-net-pci-non-transitional,netdev=net0 \
    -netdev user,id=net0 \
    -nographic \
    > "$OUTPUT_FILE" 2>&1 &
QEMU_PID=$!

# Monitor output: kill QEMU early when the shell prompt appears after tests
# (the "sbush$ " prompt means rc script finished and shell is idle)
ELAPSED=0
while kill -0 "$QEMU_PID" 2>/dev/null; do
    sleep 5
    ELAPSED=$((ELAPSED + 5))
    # Check for test completion markers
    if grep -q 'sbush\$' "$OUTPUT_FILE" 2>/dev/null; then
        # Tests finished, shell prompt appeared — give 5s for final output
        sleep 5
        echo "==> Tests completed, stopping QEMU..."
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
        QEMU_PID=""
        break
    fi
    if [ "$ELAPSED" -ge "$TIMEOUT" ]; then
        echo "==> Timeout reached (${TIMEOUT}s), stopping QEMU..."
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
        QEMU_PID=""
        break
    fi
done

echo "==> QEMU exited, parsing results..."
echo ""

# Parse [PASS] and [FAIL] lines
pass_count=0
fail_count=0
failed_tests=()

while IFS= read -r line; do
    # Extract test name: everything after [PASS] or [FAIL]
    if [[ "$line" =~ \[PASS\](.*) ]]; then
        pass_count=$((pass_count + 1))
    elif [[ "$line" =~ \[FAIL\](.*) ]]; then
        fail_count=$((fail_count + 1))
        test_name="${BASH_REMATCH[1]}"
        test_name="$(echo "$test_name" | sed 's/^[[:space:]]*//')"
        failed_tests+=("$test_name")
    fi
done < "$OUTPUT_FILE"

total=$((pass_count + fail_count))

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

if [ "$total" -eq 0 ]; then
    echo ""
    echo "FATAL: No test results found in output."
    echo "Ensure the OS boots and runs test binaries that emit [PASS]/[FAIL]."
    echo ""
    exit 1
fi

exit 0
