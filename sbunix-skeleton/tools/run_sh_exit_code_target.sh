#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_DIR"

python3 tools/run_guest_focus.py \
  --label sh-exit-code \
  --build-tests \
  --live \
  --timeout "${TIMEOUT:-120}" \
  --command "/bin/sbush -c 'false; echo SH_EXIT_\$?'" \
  --require "SH_EXIT_1" \
  --forbid "SH_EXIT_0"
