#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
THIRDPARTY_DIR="$PROJECT_DIR/thirdparty"
MODE="${1:-all}"

check_file() {
    local label="$1"
    local path="$2"
    if [ -f "$path" ]; then
        echo "[PASS] thirdparty: $label present at $path"
    else
        echo "[SKIP] thirdparty: $label missing at $path"
    fi
}

case "$MODE" in
    micropython)
        check_file "micropython" "$THIRDPARTY_DIR/micropython-riscv64"
        ;;
    pdpmake|pdpmake-build)
        check_file "pdpmake" "$THIRDPARTY_DIR/pdpmake-riscv64"
        ;;
    tcc|tcc-compile)
        check_file "tcc" "$THIRDPARTY_DIR/tcc-riscv64"
        ;;
    busybox|busybox-fs|busybox-txt)
        check_file "busybox" "$THIRDPARTY_DIR/busybox-riscv64"
        ;;
    all)
        check_file "micropython" "$THIRDPARTY_DIR/micropython-riscv64"
        check_file "pdpmake" "$THIRDPARTY_DIR/pdpmake-riscv64"
        check_file "tcc" "$THIRDPARTY_DIR/tcc-riscv64"
        check_file "busybox" "$THIRDPARTY_DIR/busybox-riscv64"
        ;;
    *)
        echo "usage: $0 [all|micropython|pdpmake|pdpmake-build|tcc|tcc-compile|busybox|busybox-fs|busybox-txt]" >&2
        exit 2
        ;;
esac
