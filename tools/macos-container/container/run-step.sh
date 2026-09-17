#!/bin/bash
# Runs one build step inside the container and records its log and exit code
# on the exchange mount so the host can observe it without an attached session.
set -u

readonly NAME="$1"
shift
readonly LOG="/mnt/exchange/logs/$NAME.log"
readonly STATE_DIR=/mnt/exchange/state

mkdir -p "$STATE_DIR"
: >"$STATE_DIR/$NAME.running"
echo "$$" >"$STATE_DIR/$NAME.pid"
echo "[$(date '+%F %T')] step $NAME started: $*" >"$LOG"

"$@" >>"$LOG" 2>&1
code=$?

echo "[$(date '+%F %T')] step $NAME exited with $code" >>"$LOG"
echo "$code" >"$STATE_DIR/$NAME.exit"
mv "$STATE_DIR/$NAME.running" "$STATE_DIR/$NAME.finished"
exit "$code"
