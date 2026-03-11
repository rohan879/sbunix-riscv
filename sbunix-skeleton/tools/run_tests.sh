#!/bin/bash
# SBUnix host-side test runner
# Builds the kernel, launches QEMU with a timeout, and parses test results.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TIMEOUT=120
OUTPUT_FILE="$(mktemp /tmp/sbunix-test-output.XXXXXX)"

cleanup() {
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

# Launch QEMU with a timeout, capturing serial output
echo "==> Launching QEMU (timeout: ${TIMEOUT}s)..."
timeout "$TIMEOUT" \
    qemu-system-riscv64 -machine virt -bios default -kernel build/kernel.elf \
        -drive file=build/disk.img,format=raw,if=none,id=hd0 \
        -device virtio-blk-pci-non-transitional,drive=hd0 \
        -device virtio-gpu-pci \
        -device virtio-net-pci-non-transitional,netdev=net0 \
        -netdev user,id=net0 \
        -nographic \
    > "$OUTPUT_FILE" 2>&1 || true

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
