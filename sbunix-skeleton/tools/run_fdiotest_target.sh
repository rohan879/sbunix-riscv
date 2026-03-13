#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_DIR"

python3 tools/run_guest_focus.py \
  --label fdiotest \
  --build-tests \
  --timeout "${TIMEOUT:-120}" \
  --command "/bin/fdiotest" \
  --require "fdiotest:" \
  --require "0 failed" \
  --forbid "[FAIL] fdio:"
