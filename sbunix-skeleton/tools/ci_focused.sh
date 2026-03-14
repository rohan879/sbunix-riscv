#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_DIR"

python3 tools/render_test_coverage.py
TIMEOUT="${TIMEOUT:-180}" bash tools/run_fdiotest_target.sh
TIMEOUT="${TIMEOUT:-180}" bash tools/run_lazytest_target.sh
TIMEOUT="${TIMEOUT:-180}" bash tools/run_sh_exit_code_target.sh
TIMEOUT="${TIMEOUT:-180}" bash tools/run_core_regressions_target.sh
TIMEOUT="${TIMEOUT:-180}" bash tools/run_shell_e2e_target.sh
TIMEOUT="${TIMEOUT:-180}" bash tools/ci_interactive_smoke.sh all
