#!/bin/bash
# Fetches Chromium at the chromium-hmos pinned revision and its DEPS, without
# history and without running hooks.
set -euo pipefail
source /mnt/exchange/scripts/env.sh

mkdir -p "$HOME" "$CHROMIUM_ROOT"
cd "$CHROMIUM_ROOT"

if [ ! -f .gclient ]; then
  cat >.gclient <<'EOF'
solutions = [
  {
    "name": "src",
    "url": "https://chromium.googlesource.com/chromium/src.git",
    "managed": False,
    "custom_deps": {},
    "custom_vars": {},
  },
]
EOF
fi

if [ ! -d src/.git ]; then
  git init -q src
  git -C src remote add origin https://chromium.googlesource.com/chromium/src.git
fi

current="$(git -C src rev-parse -q --verify HEAD 2>/dev/null || true)"
if [ "$current" != "$CHROMIUM_REVISION" ]; then
  log "Fetching src@$CHROMIUM_REVISION"
  git -C src fetch --depth=1 --progress origin "$CHROMIUM_REVISION"
  git -C src checkout -q --detach FETCH_HEAD
fi
log "src is at $(git -C src rev-parse HEAD)"

log "Syncing DEPS without history or hooks"
gclient sync -D --no-history --nohooks --jobs 8
log "Fetch complete"
du -sh "$CHROMIUM_SRC" || true
