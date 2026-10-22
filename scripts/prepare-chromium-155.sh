#!/usr/bin/env bash
# Run only inside the push-triggered self-hosted job. Never reset the 154 tree.
set -euo pipefail
[[ "${GITHUB_ACTIONS:-}" == true && "${GITHUB_REF_NAME:-}" == hmos-155-adapter ]] || {
  echo 'Chromium 155 preparation must run in CI on hmos-155-adapter' >&2; exit 1;
}
readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly root=/root/chromium-155
readonly src="${root}/src"
[[ "${CHROMIUM_SRC:-}" == "$src" ]] || { echo '155 source path mismatch' >&2; exit 1; }
readonly seed=/root/chromium-154/src
readonly revision="${CHROMIUM_REVISION:?Pinned revision is required}"
mkdir -p "$root" "$repo_root/build-status"
export PATH="$src/third_party/depot_tools:$PATH"
export DEPOT_TOOLS_UPDATE=0
export DEPOT_TOOLS_METRICS=0

# Copy dependencies and tool payloads, never old native outputs. Git object
# files are copied, not hardlinked: resets/fetches in 155 cannot affect 154.
if [[ ! -f "$root/.seed-complete" ]]; then
  mkdir -p "$src"
  rsync -a --exclude='/out/' --exclude='/.git/' "$seed/" "$src/"
  if [[ ! -d "$src/.git" ]]; then
    git clone --no-hardlinks --no-checkout "$seed" "$root/git-seed"
    mv "$root/git-seed/.git" "$src/.git"
    rmdir "$root/git-seed"
  fi
  cp /root/chromium-154/.gclient "$root/.gclient"
  # Chromium itself is available on the official GitHub mirror; all DEPS
  # revisions remain exactly those declared by the pinned Chromium version.
  git -C "$src" remote set-url origin https://github.com/chromium/chromium.git
  touch "$root/.seed-complete"
fi

if [[ ! -f "$root/.deps-$revision" ]]; then
  git -C "$src" fetch --depth=1 origin "$revision"
  git -C "$src" reset --hard "$revision"
  # gclient verifies the checkout URL against its solution configuration.
  sed -i 's#https://chromium.googlesource.com/chromium/src.git#https://github.com/chromium/chromium.git#' "$root/.gclient"
  (cd "$root" && gclient sync --nohooks --noprehooks --reset --force -D --revision "src@$revision")
  touch "$root/.deps-$revision"
fi

python3 "$repo_root/scripts/apply-155-snapshot.py" "$src" \
  2>&1 | tee "$repo_root/build-status/migration-155.log"
rsync -a --checksum "$repo_root/overlay/" "$src/"
mkdir -p "$root/deps_code/webview" "$src/out/ohos_arm64"
rsync -a --checksum "$repo_root/external/deps_code/webview/" "$root/deps_code/webview/"
if [[ ! -f "$src/out/ohos_arm64/args.gn" ]]; then
  cp "$repo_root/config/args.plan_kirin_pc.gn" "$src/out/ohos_arm64/args.gn"
fi
# The copied tools belong to 154. Hooks install the exact 155 compiler, Rust,
# GN and generated dependencies before anything is compiled.
if [[ ! -f "$root/.hooks-$revision" ]]; then
  (cd "$root" && gclient runhooks)
  touch "$root/.hooks-$revision"
fi
