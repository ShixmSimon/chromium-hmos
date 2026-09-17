#!/bin/bash
# Builds, signs, and installs the exported chromium-ui HAP, launches it once
# the phone is unlocked, then saves a screenshot and the app's hilog.
# Usage: deploy-and-capture.sh <label>
set -u

readonly LABEL="${1:?usage: $0 <label>}"
readonly BASE="${HMOS_BUILD_BASE:?Set HMOS_BUILD_BASE to the external build directory}"
readonly PROJECT="$BASE/exchange/chromium-ui"
readonly LOGS="$BASE/logs"
readonly DEVICE="${HMOS_DEVICE:?Set HMOS_DEVICE to the hdc device serial}"
readonly BUNDLE="${HMOS_BUNDLE:?Set HMOS_BUNDLE to the signed bundle name}"
readonly HDC="${HMOS_HDC:-/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc}"
readonly LAUNCH_ATTEMPTS=120
readonly PAGE_SETTLE_SECONDS=25

cd "$PROJECT" || exit 1
if ! devecocli run --product default --build-mode release --device "$DEVICE" \
  >"$LOGS/deploy-$LABEL.log" 2>&1; then
  echo "devecocli run failed; see $LOGS/deploy-$LABEL.log"
  tail -20 "$LOGS/deploy-$LABEL.log"
  exit 1
fi
if ! grep -q "App installed successfully" "$LOGS/deploy-$LABEL.log"; then
  echo "install not confirmed; see $LOGS/deploy-$LABEL.log"
  exit 1
fi
echo "$(date '+%T') installed"

launched=0
if grep -qi "start ability successfully" "$LOGS/deploy-$LABEL.log"; then
  launched=1
fi
for _ in $(seq 1 "$LAUNCH_ATTEMPTS"); do
  [ "$launched" = 1 ] && break
  if "$HDC" -t "$DEVICE" shell aa start -b "$BUNDLE" -a EntryAbility 2>&1 |
    grep -qi "start ability successfully"; then
    launched=1
    break
  fi
  sleep 10
done
if [ "$launched" != 1 ]; then
  echo "app not launched; phone may still be locked"
  exit 1
fi
echo "$(date '+%T') launched"

sleep "$PAGE_SETTLE_SECONDS"
pid=$("$HDC" -t "$DEVICE" shell pidof "$BUNDLE" | tr -d '\r')
echo "pid=${pid:-none}"
devecocli ui screenshot --device "$DEVICE" --path "$LOGS/screen-$LABEL.png" 2>&1 | tail -1
if [ -n "$pid" ]; then
  "$HDC" -t "$DEVICE" shell "hilog -x -P $pid" >"$LOGS/hilog-$LABEL.txt" 2>&1
  echo "hilog lines: $(wc -l <"$LOGS/hilog-$LABEL.txt")"
fi
