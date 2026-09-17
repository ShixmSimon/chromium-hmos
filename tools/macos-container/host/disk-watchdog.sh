#!/bin/bash
# Stops the chromium-hmos build when free disk space drops below the limits
# or falls too quickly. Never deletes files. Exits non-zero after stopping so
# the caller is notified.
set -u

readonly BASE="${HMOS_BUILD_BASE:?Set HMOS_BUILD_BASE to the external build directory}"
readonly LOG="$BASE/logs/disk-watchdog.log"
readonly STOP_FLAG="$BASE/STOPPED"
readonly CONTAINER=hmos-build
readonly INTERNAL_MOUNT=/System/Volumes/Data
readonly EXTERNAL_MOUNT="${HMOS_EXTERNAL_MOUNT:?Set HMOS_EXTERNAL_MOUNT to the external volume}"
readonly INTERNAL_MIN_GIB="${HMOS_INTERNAL_MIN_GIB:-170}"
readonly EXTERNAL_MIN_GIB="${HMOS_EXTERNAL_MIN_GIB:-600}"
readonly VOLUME_MIN_GIB="${HMOS_VOLUME_MIN_GIB:-30}"
# A sparse image copied across volumes filled 128 GiB in under 30 seconds.
readonly INTERNAL_MAX_DROP_GIB=10
readonly INTERVAL_SECONDS=5
readonly SLOW_CHECK_EVERY=6

log() {
  echo "$(date '+%F %T') $*" >>"$LOG"
}

with_timeout() {
  local seconds="$1"
  shift
  perl -e 'alarm shift; exec @ARGV' "$seconds" "$@"
}

free_gib() {
  df -k "$1" 2>/dev/null | awk 'NR==2 { printf "%d", $4 / 1048576 }'
}

volume_free_gib() {
  with_timeout 10 container exec "$CONTAINER" df -k /work 2>/dev/null |
    awk 'NR==2 { printf "%d", $4 / 1048576 }'
}

stop_build() {
  local reason="$1"
  log "STOP: $reason"
  echo "$(date '+%F %T') $reason" >"$STOP_FLAG"

  with_timeout 20 container exec "$CONTAINER" sh -c \
    'pkill -TERM -f "autoninja|ninja|siso|gclient|gn gen|cipd|git fetch|git clone|apt-get|dpkg"' \
    >/dev/null 2>&1

  local pid_file
  for pid_file in "$BASE"/run/*.pid; do
    [ -f "$pid_file" ] || continue
    kill -TERM "$(cat "$pid_file")" >/dev/null 2>&1
  done

  with_timeout 60 container stop --all >/dev/null 2>&1
  with_timeout 60 container system stop >/dev/null 2>&1
  log "Build processes, containers, and container services stopped."
}

log "Watchdog started: internal>=${INTERNAL_MIN_GIB}GiB drop<${INTERNAL_MAX_DROP_GIB}GiB/${INTERVAL_SECONDS}s external>=${EXTERNAL_MIN_GIB}GiB volume>=${VOLUME_MIN_GIB}GiB"

previous_internal_free=""
iteration=0
volume_free=""

while true; do
  if [ -f "$STOP_FLAG" ]; then
    log "Stop flag present: $(cat "$STOP_FLAG")"
    exit 2
  fi

  if [ ! -d "$EXTERNAL_MOUNT" ]; then
    stop_build "external disk not mounted"
    exit 1
  fi

  internal_free="$(free_gib "$INTERNAL_MOUNT")"
  external_free="$(free_gib "$EXTERNAL_MOUNT")"

  if [ -z "$internal_free" ] || [ -z "$external_free" ]; then
    stop_build "unable to read disk usage"
    exit 1
  fi
  if [ "$internal_free" -lt "$INTERNAL_MIN_GIB" ]; then
    stop_build "internal free ${internal_free}GiB < ${INTERNAL_MIN_GIB}GiB"
    exit 1
  fi
  if [ -n "$previous_internal_free" ] &&
    [ $((previous_internal_free - internal_free)) -ge "$INTERNAL_MAX_DROP_GIB" ]; then
    stop_build "internal free dropped ${previous_internal_free}->${internal_free}GiB in ${INTERVAL_SECONDS}s"
    exit 1
  fi
  if [ "$external_free" -lt "$EXTERNAL_MIN_GIB" ]; then
    stop_build "external free ${external_free}GiB < ${EXTERNAL_MIN_GIB}GiB"
    exit 1
  fi
  previous_internal_free="$internal_free"

  if [ $((iteration % SLOW_CHECK_EVERY)) -eq 0 ]; then
    volume_free="$(volume_free_gib)"
    log "internal=${internal_free}GiB external=${external_free}GiB volume=${volume_free:-n/a}GiB"
    if [ -n "$volume_free" ] && [ "$volume_free" -lt "$VOLUME_MIN_GIB" ]; then
      stop_build "build volume free ${volume_free}GiB < ${VOLUME_MIN_GIB}GiB"
      exit 1
    fi
  fi

  iteration=$((iteration + 1))
  sleep "$INTERVAL_SECONDS"
done
