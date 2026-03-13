#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TIMEOUT="${TIMEOUT:-120}"

cd "$PROJECT_DIR"

python3 tools/run_guest_focus.py \
  --label persist-write \
  --build-tests \
  --live \
  --timeout "$TIMEOUT" \
  --command "/bin/persisttest write" \
  --require "[PASS] persist: open for write" \
  --require "[PASS] persist: fsync write phase" \
  --forbid "[FAIL] persist:"

python3 tools/run_guest_focus.py \
  --label persist-verify \
  --build-tests \
  --live \
  --timeout "$TIMEOUT" \
  --command "/bin/persisttest verify" \
  --require "[PASS] persist: read persisted data" \
  --require "[PASS] persist: read appended data" \
  --forbid "[FAIL] persist:"
