#!/bin/bash
# Runs only the adapter's GN generation (no Ninja) to surface configuration
# errors quickly. Staging validation afterwards is expected to fail.
set -uo pipefail
source /mnt/exchange/scripts/env.sh

cd "$CHROMIUM_SRC"
CHROMIUM_SKIP_NINJA=1 bash ./ohos_build/build.sh -t n 2>&1 |
  sed -n '/Generating fixed plan_kirin_pc GN graph/,$p' | head -60
