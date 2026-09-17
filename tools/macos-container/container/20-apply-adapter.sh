#!/bin/bash
# Applies the chromium-hmos patch and overlay to the clean pinned checkout.
set -euo pipefail
source /mnt/exchange/scripts/env.sh

if [ -f "$CHROMIUM_SRC/ohos_build/build.sh" ]; then
  log "Adapter overlay already present; skipping apply"
  exit 0
fi

log "Applying chromium-hmos adapter"
"$ADAPTER_ROOT/scripts/apply-adapter.sh" "$CHROMIUM_SRC"
test -L "$CHROMIUM_SRC/ohos_nweb"
test -f "$CHROMIUM_SRC/chromium-ui/entry/src/main/ets/pages/AuraShell.ets"
log "Adapter applied"
