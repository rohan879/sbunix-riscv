#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "Usage: tools/review_summarize.sh <report-dir>" >&2
  exit 1
fi

REPORT_DIR="$1"
OUTPUT="$REPORT_DIR/final-report.md"

if [ ! -d "$REPORT_DIR" ]; then
  echo "report dir not found: $REPORT_DIR" >&2
  exit 1
fi

{
  echo "# SBUnix Review Report"
  echo
  echo "## Summary"
  echo
  echo "Consolidated review report synthesized from parallel track outputs in \`$REPORT_DIR\`."
  echo
  echo "## Findings"
  echo

  found_any=0
  while IFS= read -r file; do
    [ -s "$file" ] || continue
    found_any=1
    echo "### $(basename "$file" .md)"
    echo
    cat "$file"
    echo
  done < <(find "$REPORT_DIR" -maxdepth 1 -type f \( -name 'correctness-core.md' -o -name 'abi-and-userspace.md' -o -name 'filesystem-and-io.md' -o -name 'tests-and-docs.md' \) | sort)

  if [ "$found_any" -eq 0 ]; then
    echo "No track findings were present."
    echo
  fi

  echo "## Questions"
  echo
  echo "List unresolved intent questions here if any remain after synthesis."
  echo
  echo "## Validation Plan"
  echo
  echo "- Re-run focused checks for each P1/P2 finding."
  echo "- If test workflow issues are reported, compare documented commands with actual script behavior."
  echo "- Use tmux-driven \`make qemu\` when validating in-guest test execution."
} >"$OUTPUT"

echo "$OUTPUT"
