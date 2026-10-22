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
exec > >(tee -a "$repo_root/build-status/preparation-155.log") 2>&1
# gclient manages src/third_party/depot_tools as a DEPS checkout. Running
# from that directory lets sync remove/replace its own Python scripts.
# Keep the bootstrap tool outside src and outside the stable 154 tree.
readonly tools="$root/depot_tools"
prepare_tools() {
if [[ ! -f "$tools/.ohos-bootstrap-ready" ]]; then
  mkdir -p "$tools"
  rsync -a "$seed/third_party/depot_tools/" "$tools/"
  tools_revision=$(python3 - "$src/DEPS" <<'PY'
import re, sys
from pathlib import Path
s = Path(sys.argv[1]).read_text()
s = s[s.index("'src/third_party/depot_tools':"):]
print(re.search(r'[0-9a-f]{40}', s).group())
PY
  )
  git -C "$tools" -c gc.auto=0 fetch --depth=1 origin "$tools_revision"
  git -C "$tools" reset --hard "$tools_revision"
  touch "$tools/.ohos-bootstrap-ready"
fi
}
export PATH="$tools:$PATH"
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
  # Cancellation may interrupt Git before it renames index.lock. The shared
  # CI concurrency group excludes other source mutations; still reject a lock
  # held by a live process rather than removing it underneath that process.
  python3 - "$src" <<'PY'
from pathlib import Path
import sys
src = Path(sys.argv[1]).resolve()
assert src == Path('/root/chromium-155/src')
lock = src / '.git/index.lock'
if lock.exists():
    owners = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            for fd in (proc / 'fd').iterdir():
                try:
                    if fd.resolve() == lock:
                        owners.append(proc.name)
                except OSError:
                    pass
        except OSError:
            pass
    if owners:
        raise RuntimeError(f'155 Git index lock is held by live processes: {owners}')
    lock.unlink()
    print('Removed abandoned 155 Git index lock after a cancelled preparation')
PY
  if [[ "$(git -C "$src" rev-parse HEAD)" != "$revision" ]]; then
    git -C "$src" -c gc.auto=0 fetch --depth=1 origin "$revision"
    git -C "$src" reset --hard "$revision"
  fi
  prepare_tools
  # gclient verifies the checkout URL against its solution configuration.
  sed -i 's#https://chromium.googlesource.com/chromium/src.git#https://github.com/chromium/chromium.git#' "$root/.gclient"
  (cd "$root" && gclient sync --nohooks --noprehooks --no-history --shallow --jobs=8 --reset --force -D --revision "src@$revision")
  touch "$root/.deps-$revision"
fi

python3 "$repo_root/scripts/apply-155-snapshot.py" "$src" \
  2>&1 | tee "$repo_root/build-status/migration-155.log"
rsync -a --checksum "$repo_root/overlay/" "$src/"
mkdir -p "$root/deps_code/webview"
rsync -a --checksum "$repo_root/external/deps_code/webview/" "$root/deps_code/webview/"
# The copied tools belong to 154. Hooks install the exact 155 compiler, Rust,
# GN and generated dependencies before anything is compiled.
if [[ ! -f "$root/.hooks-$revision" ]]; then
  (cd "$root" && gclient runhooks)
  touch "$root/.hooks-$revision"
fi
# Landmine hooks may remove out/. Create arguments only after those hooks.
mkdir -p "$src/out/ohos_arm64"
if [[ ! -f "$src/out/ohos_arm64/args.gn" ]]; then
  cp "$repo_root/config/args.plan_kirin_pc.gn" "$src/out/ohos_arm64/args.gn"
fi
