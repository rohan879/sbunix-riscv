#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_DIR"

source tools/sourceme.sh
python3 tools/render_test_coverage.py
make BUILD_TESTS=1 -j2 build/kernel.elf build/disk.img
