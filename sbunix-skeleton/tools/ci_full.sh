#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_DIR"

python3 tools/render_test_coverage.py
TIMEOUT="${TIMEOUT:-300}" bash tools/run_tests.sh
TIMEOUT="${TIMEOUT:-180}" bash tools/run_persist_flow.sh
