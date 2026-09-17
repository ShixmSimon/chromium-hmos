#!/bin/bash
# Runs the chromium-hmos build steps in order inside the build container.
# Completed steps are skipped; the pipeline stops at the first failure or when
# the disk watchdog has stopped the build.
set -u

readonly BASE="${HMOS_BUILD_BASE:?Set HMOS_BUILD_BASE to the external build directory}"
readonly STATE="$BASE/exchange/state"
readonly LOGS="$BASE/exchange/logs"
readonly CONTAINER=hmos-build
readonly POLL_SECONDS=30
readonly STEPS=(
  "10-fetch ubuntu 10-fetch-chromium.sh"
  "20-apply ubuntu 20-apply-adapter.sh"
  "30-deps root 30-install-deps.sh"
  "40-hooks-sdk ubuntu 40-runhooks-and-sdk.sh"
  "50-build ubuntu 50-build-native.sh"
)

say() {
  echo "$(date '+%F %T') $*"
}

stop_if_watchdog_fired() {
  if [ -f "$BASE/STOPPED" ]; then
    say "watchdog stopped the build: $(cat "$BASE/STOPPED")"
    exit 2
  fi
}

show_failure() {
  local name="$1"
  say "$name failed with exit $(cat "$STATE/$name.exit")"
  tail -c 6000 "$LOGS/$name.log" | tr '\r' '\n' | grep -v '^\s*$' | tail -40
}

for entry in "${STEPS[@]}"; do
  read -r name user script <<<"$entry"

  if [ -f "$STATE/$name.finished" ]; then
    if [ "$(cat "$STATE/$name.exit")" = 0 ]; then
      say "$name already complete"
      continue
    fi
    show_failure "$name"
    exit 1
  fi

  if [ ! -f "$STATE/$name.running" ]; then
    stop_if_watchdog_fired
    if ! container exec -d -u "$user" "$CONTAINER" /mnt/exchange/scripts/run-step.sh \
      "$name" "/mnt/exchange/scripts/$script" >/dev/null; then
      say "failed to launch $name"
      exit 1
    fi
    say "launched $name as $user"
  else
    say "waiting for running $name"
  fi

  until [ -f "$STATE/$name.finished" ]; do
    stop_if_watchdog_fired
    sleep "$POLL_SECONDS"
  done

  if [ "$(cat "$STATE/$name.exit")" != 0 ]; then
    show_failure "$name"
    exit 1
  fi
  say "$name complete"
done

say "pipeline complete; chromium-ui exported to $BASE/exchange/chromium-ui"
