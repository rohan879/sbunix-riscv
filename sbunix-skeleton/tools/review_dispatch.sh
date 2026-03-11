#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPORT_ROOT="$REPO_DIR/docs/review/reports"
PROMPT_DIR="$REPO_DIR/docs/review/prompts"
MASTER_PROMPT="$REPO_DIR/docs/review/interactive-review-prompt.md"
MODE="${1:-full}"
shift || true

usage() {
  cat <<'EOF'
Usage:
  tools/review_dispatch.sh full
  tools/review_dispatch.sh diff
  tools/review_dispatch.sh tests
  tools/review_dispatch.sh paths <path> [<path> ...]
EOF
}

timestamp() {
  date -u +"%Y%m%dT%H%M%SZ"
}

ensure_paths() {
  mkdir -p "$REPORT_ROOT"
  test -f "$MASTER_PROMPT"
}

collect_scope_file() {
  local outfile="$1"
  case "$MODE" in
    full)
      (cd "$REPO_DIR" && rg --files kernel libc bin rootfs tools) >"$outfile"
      ;;
    diff)
      (cd "$REPO_DIR" && {
        git diff --name-only
        git diff --name-only --cached
        git ls-files --others --exclude-standard
      } | sort -u) >"$outfile"
      ;;
    tests)
      cat >"$outfile" <<'EOF'
rootfs/bin/run-tests
tools/run_tests.sh
../AGENTS.md
README.md
bin/proctest/proctest.c
bin/schedtest/schedtest.c
bin/sigpipetest/sigpipetest.c
bin/fstest/fstest.c
bin/swaptest/swaptest.c
EOF
      ;;
    paths)
      if [ "$#" -eq 0 ]; then
        echo "paths mode requires at least one path" >&2
        exit 1
      fi
      printf '%s\n' "$@" >"$outfile"
      ;;
    *)
      usage
      exit 1
      ;;
  esac
}

write_git_context() {
  local outdir="$1"
  {
    echo "# Git Status"
    echo
    (cd "$REPO_DIR" && git status --short)
    echo
    echo "# Recent Commits"
    echo
    (cd "$REPO_DIR" && git log --oneline -n 12)
  } >"$outdir/git-context.md"
}

write_test_evidence() {
  local outdir="$1"
  {
    echo "# AGENTS.md Test Workflow Excerpts"
    echo
    sed -n '240,310p' "$REPO_DIR/../AGENTS.md" || true
    echo
    echo "# rootfs/bin/run-tests"
    echo
    sed -n '1,220p' "$REPO_DIR/rootfs/bin/run-tests"
    echo
    echo "# tools/run_tests.sh"
    echo
    sed -n '1,220p' "$REPO_DIR/tools/run_tests.sh"
  } >"$outdir/tests-evidence.md"
}

write_briefing() {
  local outdir="$1"
  local track="$2"
  local prompt="$3"
  cat >"$outdir/$track-briefing.md" <<EOF
# $track Briefing

## Mode

\`$MODE\`

## Master Prompt

\`$MASTER_PROMPT\`

## Track Prompt

\`$prompt\`

## Scope File

\`$outdir/scope.txt\`

## Git Context

\`$outdir/git-context.md\`

## Suggested Review Pattern

1. Read the master prompt.
2. Read the track prompt.
3. Review the files in \`scope.txt\` relevant to this track.
4. Write findings to \`$outdir/$track.md\`.
5. Use P0-P4 severities and include validation steps.
EOF
}

main() {
  ensure_paths

  local report_dir="$REPORT_ROOT/$(timestamp)-$MODE"
  mkdir -p "$report_dir"

  collect_scope_file "$report_dir/scope.txt" "$@"
  write_git_context "$report_dir"
  write_test_evidence "$report_dir"

  write_briefing "$report_dir" "correctness-core" "$PROMPT_DIR/correctness-core.md"
  write_briefing "$report_dir" "abi-and-userspace" "$PROMPT_DIR/abi-and-userspace.md"
  write_briefing "$report_dir" "filesystem-and-io" "$PROMPT_DIR/filesystem-and-io.md"
  write_briefing "$report_dir" "tests-and-docs" "$PROMPT_DIR/tests-and-docs.md"

  echo "$report_dir"
}

main "$@"
