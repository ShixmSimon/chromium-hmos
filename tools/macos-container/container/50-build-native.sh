#!/bin/bash
# Builds the Chromium HarmonyOS native runtime, stages it into chromium-ui, and
# exports the ArkUI project to the exchange mount for HAP packaging on macOS.
set -euo pipefail
source /mnt/exchange/scripts/env.sh

readonly JOBS="${BUILD_JOBS:-10}"
readonly EXPORT_DIR=/mnt/exchange/chromium-ui

export CHROMIUM_NINJA=/mnt/exchange/scripts/ninja-keep-going.sh

cd "$CHROMIUM_SRC"
log "Building native runtime with $JOBS jobs"
# The published adapter does not mark build.sh executable.
bash ./ohos_build/build.sh -j "$JOBS" -t n

log "Exporting staged chromium-ui to $EXPORT_DIR"
mkdir -p "$EXPORT_DIR"
# The exported project keeps its own bundle name and device signing config.
rsync -a --exclude '.hvigor/' --exclude 'entry/.cxx/' --exclude 'entry/build/' \
  --exclude 'oh_modules/' --exclude '/AppScope/app.json5' \
  --exclude '/build-profile.json5' "$CHROMIUM_SRC/chromium-ui/" "$EXPORT_DIR/"
du -sh "$EXPORT_DIR"
log "Native build exported"
