#!/bin/bash
# Emits one line per notable event for a detached build step, then exits on a
# terminal condition. Usage: watch-step.sh <step-name> [max-seconds] [stall-seconds]
set -u

readonly STEP="$1"
readonly MAX_SECONDS="${2:-1740}"
readonly BASE="${HMOS_BUILD_BASE:?Set HMOS_BUILD_BASE to the external build directory}"
readonly LOG="$BASE/exchange/logs/$STEP.log"
readonly STATE="$BASE/exchange/state"
readonly WATCHDOG_LOG="$BASE/logs/disk-watchdog.log"
readonly POLL_SECONDS=60
readonly STALL_SECONDS="${3:-1200}"
readonly HEARTBEAT_SECONDS=900
readonly ERROR_PATTERN='fatal:|FAILED|Error:|error:|Traceback|No space left|Killed|out of memory|remote end hung up|Connection reset|timed out'

file_mtime() {
  stat -f %m "$1" 2>/dev/null || echo 0
}

last_progress_line() {
  tail -c 4000 "$LOG" 2>/dev/null | tr '\r' '\n' | grep -v '^\s*$' | tail -1 | cut -c1-200
}

started=$(date +%s)
last_heartbeat=$started
error_count=$(grep -cE "$ERROR_PATTERN" "$LOG" 2>/dev/null || true)
error_count=${error_count:-0}

while true; do
  now=$(date +%s)

  if [ -f "$BASE/STOPPED" ]; then
    echo "TERMINAL watchdog-stopped: $(cat "$BASE/STOPPED")"
    exit 0
  fi
  if [ -f "$STATE/$STEP.finished" ]; then
    echo "TERMINAL step-finished exit=$(cat "$STATE/$STEP.exit" 2>/dev/null)"
    exit 0
  fi
  if [ ! -f "$STATE/$STEP.running" ]; then
    echo "TERMINAL step-not-running (no running or finished marker)"
    exit 0
  fi

  if [ $((now - $(file_mtime "$WATCHDOG_LOG"))) -gt 120 ]; then
    echo "TERMINAL watchdog-stale: last log $(tail -1 "$WATCHDOG_LOG")"
    exit 0
  fi

  if [ $((now - $(file_mtime "$LOG"))) -gt "$STALL_SECONDS" ]; then
    echo "TERMINAL step-stalled: log unchanged for ${STALL_SECONDS}s; last: $(last_progress_line)"
    exit 0
  fi

  current_errors=$(grep -cE "$ERROR_PATTERN" "$LOG" 2>/dev/null || true)
  current_errors=${current_errors:-0}
  if [ "$current_errors" -gt "$error_count" ]; then
    echo "ERROR-LINES $(grep -E "$ERROR_PATTERN" "$LOG" | tail -3 | cut -c1-240 | tr '\n' '|')"
    error_count=$current_errors
  fi

  if [ $((now - last_heartbeat)) -ge "$HEARTBEAT_SECONDS" ]; then
    echo "PROGRESS $(last_progress_line) || disk: $(tail -1 "$WATCHDOG_LOG" | cut -d' ' -f3-)"
    last_heartbeat=$now
  fi

  if [ $((now - started)) -ge "$MAX_SECONDS" ]; then
    echo "REARM window elapsed; step still running"
    exit 0
  fi

  sleep "$POLL_SECONDS"
done
