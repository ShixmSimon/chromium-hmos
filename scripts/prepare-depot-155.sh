#!/usr/bin/env bash
# Bootstrap the isolated local workspace restored/saved through actions/cache.
set -euo pipefail
readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly cache=/mnt/chromium-155
readonly root="$cache/source"
readonly src="$root/src"
readonly tools="$cache/depot_tools"
readonly sdk="$cache/ohos_sdk"
readonly revision="$(python3 - "$repo_root/config/engines/155.json" <<'PYCODE'
import json, sys
print(json.load(open(sys.argv[1]))["revision"])
PYCODE
)"
case "${DEPOT_BRANCH:-}" in
  hmos-155-adapter|ci/depot-155) ;;
  *) echo 'This Depot workspace is reserved for Chromium 155.' >&2; exit 1 ;;
esac
[[ "${CHROMIUM_MAJOR:-}" == 155 && "${CHROMIUM_SRC:-}" == "$src" &&
   "${CHROMIUM_REVISION:-}" == "$revision" ]] || {
  echo 'The selected engine does not match this Depot workspace.' >&2; exit 1;
}
[[ "$(uname -m)" == x86_64 ]] || { echo 'The native build requires an x86_64 host.' >&2; exit 1; }
if mountpoint -q "$cache"; then
  echo 'Compile on the local disk; the shared cache filesystem stalls large Git writes.' >&2
  exit 1
fi
[[ -d "$cache" && ! -L "$cache" ]] || { echo 'The local Depot workspace must exist first.' >&2; exit 1; }

# Cache downloaded packages, while installing them in each fresh runner's OS.
# Caching /usr or dpkg state would skip the actual package installation.
sudo install -d -m 0755 "$cache/apt/archives/partial"
sudo tee /etc/apt/apt.conf.d/99-depot-native-cache >/dev/null <<APT
Dir::Cache::archives "$cache/apt/archives";
APT::Keep-Downloaded-Packages "true";
Binary::apt::APT::Keep-Downloaded-Packages "true";
APT
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends git curl ca-certificates python3 python3-venv \
  build-essential pkg-config rsync file unzip zip xz-utils bzip2 zstd libssl-dev
mkdir -p "$root" "$cache/sdk-inbox" "$cache/cipd" "$cache/vpython" "$cache/rustup" "$cache/cargo"
export DEPOT_TOOLS_UPDATE=0 DEPOT_TOOLS_METRICS=0
export CIPD_CACHE_DIR="$cache/cipd" VPYTHON_VIRTUALENV_ROOT="$cache/vpython"
export RUSTUP_HOME="$cache/rustup" CARGO_HOME="$cache/cargo"
export PATH="$tools:$CARGO_HOME/bin:$PATH"

if [[ ! -d "$src/.git" ]]; then
  git init -q "$src"
  git -C "$src" remote add origin https://github.com/chromium/chromium.git
fi
actual="$(git -C "$src" rev-parse --verify HEAD 2>/dev/null || true)"
if [[ -z "$actual" ]]; then
  git -C "$src" fetch --depth=1 origin "$revision"
  git -C "$src" checkout -q --detach FETCH_HEAD
elif [[ "$actual" != "$revision" ]]; then
  echo 'Cached source revision differs from the 155 pin; use a new cache disk.' >&2; exit 1
fi
tools_revision="$(python3 - "$src/DEPS" <<'PY'
from pathlib import Path
import re, sys
text = Path(sys.argv[1]).read_text().split("'src/third_party/depot_tools':", 1)[1]
print(re.search(r'[0-9a-f]{40}', text).group())
PY
)"
if [[ ! -d "$tools/.git" ]]; then
  git init -q "$tools"
  git -C "$tools" remote add origin https://chromium.googlesource.com/chromium/tools/depot_tools.git
fi
if [[ "$(git -C "$tools" rev-parse --verify HEAD 2>/dev/null || true)" != "$tools_revision" ]]; then
  git -C "$tools" fetch --depth=1 origin "$tools_revision"
  git -C "$tools" checkout -q --detach FETCH_HEAD
fi

if [[ ! -f "$root/.deps-$revision" ]]; then
  cat > "$root/.gclient" <<'GCLIENT'
solutions = [{"name": "src", "url": "https://github.com/chromium/chromium.git",
              "managed": False, "custom_deps": {}, "custom_vars": {}}]
GCLIENT
  (cd "$root" && gclient sync --nohooks --noprehooks --no-history --shallow --jobs=16 --revision "src@$revision")
  touch "$root/.deps-$revision"
fi

# OS packages belong to each ephemeral sandbox, even when source/hooks are cached.
help="$(bash "$src/build/install-build-deps.sh" --help 2>&1)"
flags=(--no-prompt)
for flag in --no-arm --no-chromeos-fonts --no-nacl --no-backwards-compatible; do
  if grep -q -- "$flag" <<< "$help"; then flags+=("$flag"); fi
done
sudo bash "$src/build/install-build-deps.sh" "${flags[@]}"
if [[ ! -f "$root/.hooks-$revision" ]]; then
  (cd "$root" && gclient runhooks)
  touch "$root/.hooks-$revision"
fi

# The SDK is uploaded over authenticated Depot exec-pipe, separately from Git.
# An atomic completion marker prevents an interrupted upload from being used.
for ((attempt=0; attempt<120; attempt++)); do
  [[ -f "$cache/sdk-inbox/sdk.ready" ]] && break
  if (( attempt % 6 == 0 )); then echo 'Waiting for the SDK upload to this Depot sandbox...'; fi
  sleep 5
done
[[ -f "$cache/sdk-inbox/sdk.ready" ]] || { echo 'SDK upload did not finish within ten minutes.' >&2; exit 1; }
python3 - "$repo_root/config/depot-155-sdk.json" "$cache/sdk-inbox/sdk.ready" "$sdk/native/oh-uni-package.json" <<'PY'
from pathlib import Path
import json, sys
expected = json.loads(Path(sys.argv[1]).read_text())
actual = json.loads(Path(sys.argv[3]).read_text())
if Path(sys.argv[2]).read_text().strip() != expected['archive_sha256']:
    raise SystemExit('SDK archive checksum does not match this build configuration')
if actual['version'] != expected['version'] or int(actual['apiVersion']) != expected['api']:
    raise SystemExit('SDK version does not match this build configuration')
PY
[[ -f "$sdk/native/oh-uni-package.json" && -d "$sdk/native/sysroot/usr/include" ]] || {
  echo 'The uploaded SDK is incomplete.' >&2; exit 1;
}
[[ -f "$sdk/native/llvm/lib/clang/current/lib/aarch64-unknown-linux-ohos/libclang_rt.builtins.a" ]] || {
  echo 'The uploaded SDK is missing the arm64 compiler runtime.' >&2; exit 1;
}
cp "$sdk/native/oh-uni-package.json" "$repo_root/build-status/sdk-version.json"

# The complete 155 snapshot replaces the legacy 154 patch replay. Its receipts
# include each patch digest, so changed snapshots are reapplied on cache hits
# while unchanged compiler inputs retain their timestamps.
python3 "$repo_root/scripts/apply-155-snapshot.py" "$src" \
  2>&1 | tee "$repo_root/build-status/migration-155.log"
rsync -a --checksum --no-times "$repo_root/overlay/" "$src/"
mkdir -p "$root/deps_code/webview"
rsync -a --checksum --no-times "$repo_root/external/deps_code/webview/" "$root/deps_code/webview/"
ln -sfn "$sdk" "$src/ohos_sdk"
# Rust's native DSO also invokes an SDK linker driver. The uploaded SDK subset
# contains target headers/libraries only; use Chromium's pinned Linux Clang
# with those SDK runtimes rather than uploading a macOS host executable.
mkdir -p "$sdk/native/llvm/bin"
cat > "$sdk/native/llvm/bin/clang" <<'CLANG'
#!/usr/bin/env bash
set -euo pipefail
exec /mnt/chromium-155/source/src/third_party/llvm-build/Release+Asserts/bin/clang \
  -resource-dir /mnt/chromium-155/ohos_sdk/native/llvm/lib/clang/current \
  -L/mnt/chromium-155/ohos_sdk/native/llvm/lib/aarch64-linux-ohos "$@"
CLANG
chmod +x "$sdk/native/llvm/bin/clang"
mkdir -p "$src/out/ohos_arm64"
if [[ ! -f "$src/out/ohos_arm64/args.gn" ]]; then
  cp "$repo_root/config/args.plan_kirin_pc.gn" "$src/out/ohos_arm64/args.gn"
fi

{
  echo "CHROMIUM_SRC=$src"
  echo 'OUT_DIR=out/ohos_arm64'
  echo "CIPD_CACHE_DIR=$CIPD_CACHE_DIR"
  echo "VPYTHON_VIRTUALENV_ROOT=$VPYTHON_VIRTUALENV_ROOT"
  echo "RUSTUP_HOME=$RUSTUP_HOME"
  echo "CARGO_HOME=$CARGO_HOME"
  echo 'DEPOT_TOOLS_UPDATE=0'
  echo 'DEPOT_TOOLS_METRICS=0'
} >> "${GITHUB_ENV:?}"
printf '%s\n' "$tools" "$CARGO_HOME/bin" >> "${GITHUB_PATH:?}"
echo "Prepared Chromium $revision for a full native build."
df -h "$cache" "$src"
