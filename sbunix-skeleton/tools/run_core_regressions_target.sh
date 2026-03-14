#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_DIR"

python3 tools/run_guest_focus.py \
  --label core-regressions \
  --build-tests \
  --live \
  --timeout "${TIMEOUT:-180}" \
  --command /bin/exectest \
  --command /bin/procfstest \
  --command /bin/pgrptest \
  --command /bin/symlinktest \
  --command /bin/clocktest \
  --require "exectest: 6 passed, 0 failed" \
  --require "procfstest: 15 passed, 0 failed" \
  --require "pgrptest: 12 passed, 0 failed" \
  --require "symlinktest: 12 passed, 0 failed" \
  --require "clocktest: 7 passed, 0 failed" \
  --forbid "[FAIL] exec:" \
  --forbid "[FAIL] procfs:" \
  --forbid "[FAIL] pgrp:" \
  --forbid "[FAIL] symlink:" \
  --forbid "[FAIL] clock:"
