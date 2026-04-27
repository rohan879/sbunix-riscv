#!/bin/bash
# Preserve per-boot raw logs because tools/run_tests.sh deletes its private
# capture file on exit, which is not sufficient for the final CR08 evidence.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_DIR="$(git -C "$PROJECT_DIR" rev-parse --show-toplevel 2>/dev/null || echo "$PROJECT_DIR")"
TIMEOUT=${TIMEOUT:-7200}
QEMU_MEM=${QEMU_MEM:-256M}
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
PHASE_FAILED=0

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

mark_phase_failed() {
    local message="$1"
    echo "FATAL: $message"
    PHASE_FAILED=1
}

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

IMPLEMENTATION_SHA="$(git -C "$REPO_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
echo "==> Validation implementation SHA: $IMPLEMENTATION_SHA"
if [ "${ALLOW_DIRTY_VALIDATION:-0}" != "1" ] &&
   [ -n "$(git -C "$REPO_DIR" status --porcelain -- sbunix-skeleton 2>/dev/null)" ]; then
    # BF: Capture logs only from a committed implementation unless a
    # development run explicitly opts into dirty-tree validation.
    echo "FATAL: sbunix-skeleton has uncommitted changes; set ALLOW_DIRTY_VALIDATION=1 for development runs."
    exit 1
fi

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

    wait_for_pattern_since() {
        local start_size="$1"
        local pattern="$2"
        while kill -0 "$QEMU_PID" 2>/dev/null; do
            sleep 5
            if tail -c +"$((start_size + 1))" "$boot_log" 2>/dev/null | grep -F -q "$pattern"; then
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
            return 1
        fi

        echo "==> $label: initial prompt reached"

        start_size=$(wc -c < "$boot_log" 2>/dev/null || echo 0)
        echo "==> $label: running Sortix suite..."
        printf 'run-os-test; echo __STATUS:sortix:$?\n' >&"$QEMU_STDIN_FD"
        if ! wait_for_pattern_since "$start_size" "__STATUS:sortix:"; then
            cat "$boot_log" >> "$COMBINED_LOG"
            return 1
        fi
        tail -c +"$((start_size + 1))" "$boot_log" 2>/dev/null |
            tr -d '\r' |
            sed -n 's/.*__STATUS:sortix:\([0-9][0-9]*\).*/==> Sortix suite exited with status \1/p' |
            tail -n 1
        if ! wait_for_pattern_since "$start_size" 'sbush$'; then
            cat "$boot_log" >> "$COMBINED_LOG"
            return 1
        fi

        start_size=$(wc -c < "$boot_log" 2>/dev/null || echo 0)
        echo "==> $label: running Open POSIX suite..."
        printf '\n/bin/run-open-posix-test; echo __STATUS:open-posix:$?\n' >&"$QEMU_STDIN_FD"
        if ! wait_for_pattern_since "$start_size" "__STATUS:open-posix:"; then
            cat "$boot_log" >> "$COMBINED_LOG"
            return 1
        fi
        tail -c +"$((start_size + 1))" "$boot_log" 2>/dev/null |
            tr -d '\r' |
            sed -n 's/.*__STATUS:open-posix:\([0-9][0-9]*\).*/==> Open POSIX suite exited with status \1/p' |
            tail -n 1
        if ! wait_for_pattern_since "$start_size" 'sbush$'; then
            cat "$boot_log" >> "$COMBINED_LOG"
            return 1
        fi

        echo "==> $label: running persist write phase..."
        printf '\n/test/bin/persisttest\n' >&"$QEMU_STDIN_FD"
        while kill -0 "$QEMU_PID" 2>/dev/null; do
            sleep 5
            if [ "$SECONDS" -ge "$BOOT_DEADLINE" ]; then
                echo "==> $label: timeout reached (${TIMEOUT}s), stopping QEMU..."
                kill "$QEMU_PID" 2>/dev/null || true
                wait "$QEMU_PID" 2>/dev/null || true
                QEMU_PID=""
                cat "$boot_log" >> "$COMBINED_LOG"
                return 1
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
                cat "$boot_log" >> "$COMBINED_LOG"
                return 1
            fi
        done
    fi

    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=""
    cat "$boot_log" >> "$COMBINED_LOG"
    return 0
}

# Boot 1: rc runs the in-tree suite, then the wrapper drives the full Sortix
# and open-posix suites plus the persist write phase from the interactive shell.
if ! run_qemu_boot "Boot 1: tests + persist write" "yes" "$BOOT1_LOG"; then
    mark_phase_failed "Boot 1 did not complete all required phases"
fi

# Boot 2: persist verify only (rc detects sentinel, runs persisttest, poweroffs).
if ! run_qemu_boot "Boot 2: persist verify" "no" "$BOOT2_LOG"; then
    mark_phase_failed "Boot 2 did not complete the persistence verify phase"
fi

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

require_log_marker() {
    local file="$1"
    local pattern="$2"
    local label="$3"

    if ! grep -q "$pattern" "$file"; then
        mark_phase_failed "missing required output marker: $label"
    fi
}

require_log_marker "$BOOT1_LOG" "=== Summary ===" "guest-run-tests summary"
require_log_marker "$BOOT1_LOG" "=== Sortix os-test Suite ===" "Sortix suite header"
require_log_marker "$BOOT1_LOG" "=== Open POSIX Test Suite ===" "Open POSIX suite header"
require_log_marker "$BOOT1_LOG" "persist: write phase complete" "persistence write completion"
require_log_marker "$BOOT2_LOG" "persist: verify phase complete" "persistence verify completion"

final_results_count=$(grep -c "=== Final Results ===" "$BOOT1_LOG" 2>/dev/null || true)
if [ "$final_results_count" -lt 2 ]; then
    # BF: A status marker alone is not enough; the full suites must reach
    # final summaries before their nonzero statuses are interpretable.
    mark_phase_failed "expected both Sortix and Open POSIX final summaries"
fi

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

if [ "$PHASE_FAILED" -ne 0 ]; then
    echo ""
    echo "FATAL: validation phases were incomplete or missing required markers."
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
