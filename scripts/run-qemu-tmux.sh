#!/usr/bin/env bash
set -euo pipefail

# Run sbunix QEMU inside tmux, trigger run-os-test on boot completion,
# wait for interactive prompt, and capture full QEMU output to a file.
#
# Usage: ./scripts/run-qemu-tmux.sh [PROJECT_DIR] [OUTPUT_FILE]
# PROJECT_DIR: directory where to run make (default: current directory)
# OUTPUT_FILE: where to write the captured QEMU output (default: ./qemu-output-<ts>.log)

PROJECT_DIR="${1:-$(pwd)}"
OUTFILE="${2:-./qemu-output-$(date +%Y%m%d-%H%M%S).log}"

# tmux socket/session names (unique per PID)
TMUX_SOCKET="/tmp/tmux-qemu-$$.sock"
SESSION="qemu"

BOOT_TIMEOUT=${BOOT_TIMEOUT:-180}   # seconds to wait for "Boot Testing Complete" (build+qemu)
PROMPT_TIMEOUT=${PROMPT_TIMEOUT:-60} # seconds to wait for sbush$ prompt after run-os-test
LS_TIMEOUT=${LS_TIMEOUT:-5} # seconds to wait for ls output after sending ls at prompt
POLL_INTERVAL=0.5
PROMPT_COOLDOWN=${PROMPT_COOLDOWN:-1} # seconds to wait after seeing prompt before interacting

cleanup() {
  if tmux -S "$TMUX_SOCKET" has-session -t "$SESSION" 2>/dev/null; then
    tmux -S "$TMUX_SOCKET" kill-session -t "$SESSION" >/dev/null 2>&1 || true
  fi
  rm -f "$TMUX_SOCKET" 2>/dev/null || true
}

trap cleanup EXIT

echo "Project dir: $PROJECT_DIR"
echo "Capturing QEMU output to: $OUTFILE"

# Build first (outside tmux) so we can capture only QEMU runtime output.
echo "Building project (make clean && BUILD_TESTS=1 make -j16) with timeout ${BOOT_TIMEOUT}s..."
BUILD_LOG=$(mktemp)
if ! timeout ${BOOT_TIMEOUT}s bash -lc "cd \"$PROJECT_DIR\" && make clean && BUILD_TESTS=1 make -j16" > "$BUILD_LOG" 2>&1; then
  echo "Build failed or timed out. Saving build log to ${OUTFILE}.build.log"
  cat "$BUILD_LOG" > "${OUTFILE}.build.log" || true
  rm -f "$BUILD_LOG"
  exit 2
fi
rm -f "$BUILD_LOG"

# Start detached tmux session (interactive shell) so we can run QEMU and capture its output
echo "Starting tmux session ($SESSION) with socket $TMUX_SOCKET..."
tmux -S "$TMUX_SOCKET" new-session -d -s "$SESSION" -c "$PROJECT_DIR" -- bash
tmux -S "$TMUX_SOCKET" set-option -t "$SESSION" history-limit 200000


echo "Trigger detected. Preparing to capture QEMU runtime output and then sending 'run-os-test' to QEMU shell..."

# Prepare a temporary output that will contain runtime output starting at the boot marker
OUT_TMP=$(mktemp)
tmp_history=$(mktemp)
tmux -S "$TMUX_SOCKET" capture-pane -p -J -S - -t "$SESSION":0 > "$tmp_history" || true
# find marker line to trim off build output
start_line=$(grep -n -m1 -E "SBUnix booting|Starting tests in:|Boot Testing Complete|sbush\$" "$tmp_history" | cut -d: -f1 || true)
if [ -n "$start_line" ]; then
  tail -n +"$start_line" "$tmp_history" > "$OUT_TMP" || true
else
  cat "$tmp_history" > "$OUT_TMP" || true
fi
rm -f "$tmp_history" || true

# Start capturing new pane output into OUT_TMP. Prefer tmux pipe-pane which appends new data.
CAP_PID=""
PIPE_ACTIVE=0
if tmux -S "$TMUX_SOCKET" pipe-pane -o -t "$SESSION":0 "cat >> \"$OUT_TMP\"" 2>/dev/null; then
  PIPE_ACTIVE=1
else
  # fall back to a snapshot loop when pipe-pane is not available
  (
    while :; do
      tmux -S "$TMUX_SOCKET" capture-pane -p -J -S - -t "$SESSION":0 > "$OUT_TMP" 2>/dev/null || true
      sleep 0.2
    done
  ) &
  CAP_PID=$!
fi

# Start QEMU inside the tmux session (the build was done earlier)
echo "Launching QEMU inside tmux (make qemu)"
tmux -S "$TMUX_SOCKET" send-keys -t "$SESSION" C-m "BUILD_TESTS=1 make qemu" C-m

# Wait for the boot trigger text inside the runtime capture (BOOT_TIMEOUT)
echo "Waiting up to ${BOOT_TIMEOUT}s for 'Boot Testing Complete' in QEMU output..."
start_ts=$(date +%s)
boot_seen=0
while :; do
  if [ -f "$OUT_TMP" ]; then
    if grep -a -m1 "Boot Testing Complete" "$OUT_TMP" >/dev/null 2>&1; then
      boot_seen=1
      break
    fi
  fi
  now=$(date +%s)
  if (( now - start_ts >= BOOT_TIMEOUT )); then
    break
  fi
  sleep "$POLL_INTERVAL"
done

if (( boot_seen == 0 )); then
  echo "Timed out waiting for 'Boot Testing Complete' after ${BOOT_TIMEOUT}s. Saving captured output and exiting."
  # stop background capture
  if [ -n "${CAP_PID-}" ]; then
    if ps -p "$CAP_PID" >/dev/null 2>&1; then
      kill "$CAP_PID" 2>/dev/null || true
      wait "$CAP_PID" 2>/dev/null || true
    fi
  fi
  if [ -f "$OUT_TMP" ]; then
    mv "$OUT_TMP" "$OUTFILE" || true
    double_free_count=$(grep -i -o "double free" "$OUTFILE" | wc -l || true)
    trap_count=$(grep -i -o "trap" "$OUTFILE" | wc -l || true)
    {
      echo
      echo "--- Phrase counts ---"
      echo "double free: $double_free_count"
      echo "trap: $trap_count"
    } >> "$OUTFILE"
    echo "Captured runtime output saved to $OUTFILE"
  else
    echo "No runtime capture file found. No output written."
  fi
  exit 2
fi

# Boot trigger seen: send run-os-test into the tmux session
echo "Boot Testing Complete detected. Sending 'run-os-test' to QEMU shell..."
tmux -S "$TMUX_SOCKET" send-keys -t "$SESSION" C-m "run-os-test" C-m

RUN_COMPLETE_MARKER=${RUN_COMPLETE_MARKER:-"=== Final Results ==="}
RUN_TIMEOUT=${RUN_TIMEOUT:-60} # seconds to wait for run-os-test to finish (default 1 minute)

echo "Waiting up to ${RUN_TIMEOUT}s for run completion marker '${RUN_COMPLETE_MARKER}' (monitoring runtime capture)..."
start_ts=$(date +%s)
run_complete=0
while :; do
  if [ -f "$OUT_TMP" ]; then
    if grep -a -m1 "$RUN_COMPLETE_MARKER" "$OUT_TMP" >/dev/null 2>&1; then
      run_complete=1
      break
    fi
  fi
  now=$(date +%s)
  if (( now - start_ts >= RUN_TIMEOUT )); then
    break
  fi
  sleep "$POLL_INTERVAL"
done

if (( run_complete == 0 )); then
  echo "Run completion marker not seen after ${RUN_TIMEOUT}s. Falling back to waiting ${PROMPT_TIMEOUT}s for sbush prompt..."
  # fallback: wait for sbush prompt for PROMPT_TIMEOUT seconds
  start_ts=$(date +%s)
  prompt_seen=0
  while :; do
    if [ -f "$OUT_TMP" ] && grep -a -m1 "sbush" "$OUT_TMP" >/dev/null 2>&1; then
      prompt_seen=1
      break
    fi
    now=$(date +%s)
    if (( now - start_ts >= PROMPT_TIMEOUT )); then
      break
    fi
    sleep "$POLL_INTERVAL"
  done
else
  echo "Run completion marker detected. Now waiting up to ${PROMPT_TIMEOUT}s for the shell prompt (sbush$)..."
  # after run completes, wait for shell prompt to reappear
  start_ts=$(date +%s)
  prompt_seen=0
  while :; do
    if [ -f "$OUT_TMP" ] && grep -a -m1 "sbush" "$OUT_TMP" >/dev/null 2>&1; then
      prompt_seen=1
      break
    fi
    now=$(date +%s)
    if (( now - start_ts >= PROMPT_TIMEOUT )); then
      break
    fi
    sleep "$POLL_INTERVAL"
  done
fi

if (( prompt_seen == 1 )); then
  echo "Prompt detected. cooling down ${PROMPT_COOLDOWN}s before interacting (to avoid racing output)..."
  sleep "$PROMPT_COOLDOWN"
  # re-confirm prompt still present after cooldown
  if [ -f "$OUT_TMP" ] && ! grep -a -m1 "sbush" "$OUT_TMP" >/dev/null 2>&1; then
    echo "Prompt no longer present after cooldown; aborting ls and saving output."
    # stop capture and save
    if [ -n "${CAP_PID-}" ]; then
      if ps -p "$CAP_PID" >/dev/null 2>&1; then
        kill "$CAP_PID" 2>/dev/null || true
        wait "$CAP_PID" 2>/dev/null || true
      fi
    fi
    if (( PIPE_ACTIVE == 1 )); then
      tmux -S "$TMUX_SOCKET" pipe-pane -t "$SESSION":0 || true
    fi
    PROC_TMP=$(mktemp)
    if command -v perl >/dev/null 2>&1; then
      perl -pe 's/\r/\n/g; 1 while s/.\x08//g' "$OUT_TMP" > "$PROC_TMP" || cp "$OUT_TMP" "$PROC_TMP" || true
    else
      awk '{ gsub(/\r/, "\n"); print }' "$OUT_TMP" > "$PROC_TMP" || cp "$OUT_TMP" "$PROC_TMP" || true
      tr -d '\010' < "$PROC_TMP" > "${PROC_TMP}.nb" || true
      mv "${PROC_TMP}.nb" "$PROC_TMP" || true
    fi
    mv "$PROC_TMP" "$OUTFILE"
    double_free_count=$(grep -i -o "double free" "$OUTFILE" | wc -l || true)
    trap_count=$(grep -i -o "trap" "$OUTFILE" | wc -l || true)
    {
      echo
      echo "--- Phrase counts ---"
      echo "double free: $double_free_count"
      echo "trap: $trap_count"
    } >> "$OUTFILE"
    echo "Prompt disappeared after cooldown; saved output to $OUTFILE"
    exit 0
  fi

  echo "Prompt detected. Sending 'ls' to the shell and waiting up to ${LS_TIMEOUT}s for output (monitoring runtime capture)..."
  # capture length before sending ls so we can wait for new output in OUT_TMP
  prev_len=0
  if [ -f "$OUT_TMP" ]; then
    prev_len=$(wc -c < "$OUT_TMP" || echo 0)
  fi
  tmux -S "$TMUX_SOCKET" send-keys -t "$SESSION" C-m "ls" C-m
  # wait until OUT_TMP grows or timeout
  start_ts=$(date +%s)
  while :; do
    cur_len=0
    if [ -f "$OUT_TMP" ]; then
      cur_len=$(wc -c < "$OUT_TMP" || echo 0)
    fi
    if (( cur_len > prev_len )); then
      break
    fi
    now=$(date +%s)
    if (( now - start_ts >= LS_TIMEOUT )); then
      break
    fi
    sleep 0.2
  done

  # give a brief moment for any remaining output to arrive
  sleep 0.5
  # Stop capture: kill background cat if present, or disable pipe-pane
  if [ -n "${CAP_PID-}" ]; then
    if ps -p "$CAP_PID" >/dev/null 2>&1; then
      kill "$CAP_PID" 2>/dev/null || true
      wait "$CAP_PID" 2>/dev/null || true
    fi
  fi
  if (( PIPE_ACTIVE == 1 )); then
    tmux -S "$TMUX_SOCKET" pipe-pane -t "$SESSION":0 || true
  fi

  # Post-process captured output to remove carriage-returns (fix line-overwrite corruption)
  PROC_TMP=$(mktemp)
  # Convert CR to newline and remove backspace overstrike sequences to fix terminal overwrite artifacts
  if command -v perl >/dev/null 2>&1; then
    perl -pe 's/\r/\n/g; 1 while s/.\x08//g' "$OUT_TMP" > "$PROC_TMP" || cp "$OUT_TMP" "$PROC_TMP" || true
  else
    # Fallback: replace CR with newline and strip backspace chars
    awk '{ gsub(/\r/, "\n"); print }' "$OUT_TMP" > "$PROC_TMP" || cp "$OUT_TMP" "$PROC_TMP" || true
    tr -d '\010' < "$PROC_TMP" > "${PROC_TMP}.nb" || true
    mv "${PROC_TMP}.nb" "$PROC_TMP" || true
  fi
  mv "$PROC_TMP" "$OUTFILE"
  rm -f "$OUT_TMP" || true

  # Count occurrences of phrases
  double_free_count=$(grep -i -o "double kfree" "$OUTFILE" | wc -l || true)
  trap_count=$(grep -i -o "trap" "$OUTFILE" | wc -l || true)
  {
    echo
    echo "--- Phrase counts ---"
    echo "double kfree: $double_free_count"
    echo "trap: $trap_count"
  } >> "$OUTFILE"

  echo "Prompt detected and ls run. Output saved to $OUTFILE"
  exit 0
else
  echo "Prompt not detected within ${PROMPT_TIMEOUT}s. Saving any captured runtime output to $OUTFILE"
  # Stop capture and move file if present
  if [ -n "${CAP_PID-}" ]; then
    if ps -p "$CAP_PID" >/dev/null 2>&1; then
      kill "$CAP_PID" 2>/dev/null || true
      wait "$CAP_PID" 2>/dev/null || true
    fi
  fi
  if (( PIPE_ACTIVE == 1 )); then
    tmux -S "$TMUX_SOCKET" pipe-pane -t "$SESSION":0 || true
  fi
  if [ -f "$OUT_TMP" ]; then
    PROC_TMP=$(mktemp)
    if command -v perl >/dev/null 2>&1; then
      perl -pe 's/\r/\n/g; 1 while s/.\x08//g' "$OUT_TMP" > "$PROC_TMP" || cp "$OUT_TMP" "$PROC_TMP" || true
    else
      awk '{ gsub(/\r/, "\n"); print }' "$OUT_TMP" > "$PROC_TMP" || cp "$OUT_TMP" "$PROC_TMP" || true
      tr -d '\010' < "$PROC_TMP" > "${PROC_TMP}.nb" || true
      mv "${PROC_TMP}.nb" "$PROC_TMP" || true
    fi
    mv "$PROC_TMP" "$OUTFILE" || true
    double_free_count=$(grep -i -o "double free" "$OUTFILE" | wc -l || true)
    trap_count=$(grep -i -o "trap" "$OUTFILE" | wc -l || true)
    {
      echo
      echo "--- Phrase counts ---"
      echo "double free: $double_free_count"
      echo "trap: $trap_count"
    } >> "$OUTFILE"
    echo "Captured runtime output saved to $OUTFILE"
  else
    echo "No runtime capture file found. No output written."
  fi
  exit 3
fi
