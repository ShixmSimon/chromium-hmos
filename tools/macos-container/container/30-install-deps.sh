#!/bin/bash
# Installs Chromium's Linux host build dependencies. Runs as root.
set -euo pipefail

# Root must not share the build user's HOME or caches on /work.
log() {
  echo "[$(date '+%F %T')] $*"
}

export DEBIAN_FRONTEND=noninteractive
readonly SCRIPT=/work/chromium/src/build/install-build-deps.sh
readonly HELP="$("$SCRIPT" --help 2>&1 || true)"

flags=(--no-prompt)
for flag in --no-arm --no-chromeos-fonts --no-nacl --no-backwards-compatible; do
  if grep -q -- "$flag" <<<"$HELP"; then
    flags+=("$flag")
  fi
done

log "Running install-build-deps.sh ${flags[*]}"
"$SCRIPT" "${flags[@]}"
apt-get install -y --no-install-recommends binutils unzip
apt-get clean
log "Dependencies installed"
