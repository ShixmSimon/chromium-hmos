#!/bin/bash
# Prints a one-screen summary of the chromium-hmos build.
# Usage: progress.sh          (once)
#        progress.sh --watch  (refresh every 30 seconds)
set -u

readonly BASE="${HMOS_BUILD_BASE:?Set HMOS_BUILD_BASE to the external build directory}"
readonly BUILD_LOG="$BASE/exchange/logs/50-build.log"

summary() {
  echo "== $(date '+%F %T') =="

  echo "-- Pipeline"
  tail -3 "$BASE/logs/pipeline.log" 2>/dev/null

  echo "-- Compile"
  local line done total seconds
  line=$(grep -E '^\[[0-9]+/[0-9]+ ' "$BUILD_LOG" 2>/dev/null | tail -1)
  if [ -n "$line" ]; then
    done=$(sed -E 's/^\[([0-9]+)\/.*/\1/' <<<"$line")
    total=$(sed -E 's/^\[[0-9]+\/([0-9]+) .*/\1/' <<<"$line")
    seconds=$(sed -E 's/^\[[0-9]+\/[0-9]+ ([0-9.]+)\].*/\1/' <<<"$line")
    awk -v d="$done" -v t="$total" -v s="$seconds" 'BEGIN {
      rate = s > 0 ? d / s * 60 : 0
      eta = rate > 0 ? (t - d) / rate : 0
      printf "%d/%d (%.1f%%), elapsed %.0f min, %.0f steps/min, naive ETA %.0f min\n",
        d, t, d * 100 / t, s / 60, rate, eta
    }'
    echo "latest: $(cut -c1-120 <<<"$line")"
  else
    echo "not started"
  fi
  failed=$(grep -c '^FAILED:' "$BUILD_LOG" 2>/dev/null || true)
  echo "failed targets: ${failed:-0}"

  echo "-- Disk (watchdog)"
  tail -1 "$BASE/logs/disk-watchdog.log"
  if [ -f "$BASE/STOPPED" ]; then
    echo "WATCHDOG STOPPED THE BUILD: $(cat "$BASE/STOPPED")"
  fi
}

if [ "${1:-}" = "--watch" ]; then
  while true; do
    clear
    summary
    sleep 30
  done
else
  summary
fi
