#!/usr/bin/env bash
set -euo pipefail
readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly src="${CHROMIUM_SRC:?}"
[[ "$src" == /mnt/chromium-154/source/src ]] || { echo 'Unexpected Depot source path.' >&2; exit 1; }
readonly out="$src/${OUT_DIR:-out/ohos_arm64}"
readonly dest="$repo_root/artifacts/depot-native"
readonly stage="$repo_root/artifacts/depot-stage"
mkdir -p "$dest" "$stage"
bash "$repo_root/scripts/stage-runtime-assets.sh" "$out" "$stage"
tar -C "$stage" -czf "$dest/chromium-154-native.tar.gz" engine
gzip -1 -c "$out/exe.unstripped/libweb_engine.so" > "$dest/libweb_engine.unstripped.so.gz"
cp "$out/args.gn" "$dest/args.gn"
cp "$repo_root/build-status/latest.json" "$dest/build.json"
cp "$repo_root/build-status/sdk-version.json" "$dest/sdk-version.json"
(
  cd "$dest"
  sha256sum chromium-154-native.tar.gz libweb_engine.unstripped.so.gz args.gn build.json sdk-version.json > SHA256SUMS
)
