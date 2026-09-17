#!/bin/bash
# Keeps building after failures so one pass reports every compile error.
export NINJA_STATUS="[%f/%t %e] "
exec /work/chromium/src/third_party/ninja/ninja -k 0 "$@"
