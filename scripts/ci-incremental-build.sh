#!/usr/bin/env bash
# Copyright (c) 2026
# Licensed under the Apache License, Version 2.0.
#
# Incremental CI build against a Chromium tree that already exists on the
# runner.
#
# self-hosted-build-hap.sh fetches Chromium from scratch, which is right for a
# release build and useless for iteration: ~50 GB of download and a full
# compile before you learn that a guard is in the wrong place. This reuses the
# working tree, syncs only what the repository changed, and lets ninja decide
# what to rebuild -- minutes instead of hours.
#
# What it can and cannot do:
#   overlay/  files are copied into the tree. Changing one recompiles the
#             translation units that include it.
#   The main patches are NOT re-applied. Doing that safely means resetting the
#   tree to pristine and rebuilding everything. Small additive patches listed
#   below are applied idempotently so a new source file can join an existing
#   target without requiring a manual runner edit.
#
# Usage:
#   scripts/ci-incremental-build.sh            # sync overlay, build, package
#   SKIP_PACKAGE=1 scripts/ci-incremental-build.sh   # build only
#
# Environment:
#   CHROMIUM_SRC   default /root/chromium-154/src
#   OUT_DIR        default out/ohos_arm64
#   NINJA_JOBS     default 16

set -uo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly src="${CHROMIUM_SRC:-/root/chromium-154/src}"
readonly out="${OUT_DIR:-out/ohos_arm64}"
readonly jobs="${NINJA_JOBS:-16}"
readonly status_dir="${repo_root}/build-status"
readonly log="${src}/../ci-build.log"

mkdir -p "$status_dir"

die() { printf '%s\n' "$*" >&2; exit 1; }
say() { printf '== %s\n' "$*"; }

[[ -d "$src" ]] || die "no Chromium tree at $src"
[[ -f "${src}/${out}/args.gn" ]] || die "no GN output at ${src}/${out}"

apply_incremental_patch() {
  local patch="$1"
  local name="${patch##*/}"
  if git -C "$src" apply --check "$patch"; then
    git -C "$src" apply "$patch" || die "failed to apply ${name}"
    say "applied incremental patch ${name}"
    return
  fi
  if git -C "$src" apply --reverse --check "$patch"; then
    say "incremental patch ${name} already applied"
    return
  fi
  die "incremental patch ${name} neither applies nor is already present"
}

apply_incremental_patch "${repo_root}/patches/ohos-audio-input.patch"
apply_incremental_patch "${repo_root}/patches/ohos-vibration.patch"
apply_incremental_patch "${repo_root}/patches/ohos-battery.patch"
apply_incremental_patch "${repo_root}/patches/ohos-wake-lock.patch"

# ---- sync overlay into the tree -------------------------------------------
# The overlay holds whole files the adapter adds or replaces. Copying by
# content (not timestamp) keeps ninja from rebuilding files that did not
# actually change -- a blanket cp -a would touch everything and cost a full
# rebuild on every run.
say 'syncing overlay'
changed=0
while IFS= read -r -d '' f; do
  rel="${f#"${repo_root}/overlay/"}"
  case "$rel" in
    chromium-ui/*|ohos_arkweb_playground/*) continue ;;  # app side, not Chromium
    arkweb/ohos_nweb/*) ;;  # Chromium's //ohos_nweb integration
    arkweb/*) continue ;;
  esac
  dst="${src}/${rel}"
  # The tree has symlinks where the overlay has plain files (ohos_glue,
  # ohos_nweb are links to real directories; git on Windows checked them out as
  # text). Writing through the link would land on whatever it points at, so
  # leave those alone -- they are provided by the tree, not the overlay.
  if [[ -L "$dst" ]]; then
    continue
  fi
  # Compare with line endings normalised. The repository is checked out on
  # Windows and carries CRLF; the tree is LF. A byte-wise compare calls every
  # file different, rewrites all 126 of them, and that alone regenerates ninja
  # and forces a full rebuild -- the opposite of what this script is for.
  if [[ ! -f "$dst" ]] || ! diff -q <(tr -d '\r' <"$f") <(tr -d '\r' <"$dst") >/dev/null 2>&1; then
    mkdir -p "$(dirname "$dst")"
    tr -d '\r' <"$f" >"$dst"
    printf '   %s\n' "$rel"
    changed=$((changed + 1))
  fi
done < <(find "${repo_root}/overlay" -type f -print0)
say "$changed overlay file(s) updated"

# ---- build -----------------------------------------------------------------
say "building (-j ${jobs})"
start=$(date +%s)
( cd "$src" && third_party/ninja/ninja -C "$out" -j "$jobs" -k 0 gn_all ) >"$log" 2>&1
rc=$?
elapsed=$(( $(date +%s) - start ))

steps=$(tr '\r' '\n' <"$log" | grep -cE '^\[[0-9]+/' || true)
fails=$(tr '\r' '\n' <"$log" | grep -c '^FAILED:' || true)

# Distil the log. The raw file runs to tens of megabytes; what a reader needs
# is which targets failed and the compiler's own diagnostics, deduplicated.
{
  printf '# build %s\n' "$(date -Is)"
  printf '# commit %s\n' "$(git -C "$repo_root" rev-parse --short HEAD 2>/dev/null || echo '?')"
  printf '# steps=%s failures=%s elapsed=%ss rc=%s\n\n' "$steps" "$fails" "$elapsed" "$rc"
  if (( fails > 0 )); then
    printf '## failed targets\n'
    tr '\r' '\n' <"$log" | grep '^FAILED:' | sed 's/^FAILED: //' | cut -c1-110 | sort -u
    printf '\n## diagnostics\n'
    tr '\r' '\n' <"$log" \
      | grep -E '^\.\./\.\.[^ ]*: *(error|fatal error): |undefined symbol:' \
      | sed -E 's#^\.\./\.\./##' | sort -u | head -60
  fi
} >"${status_dir}/errors.txt"

if (( rc != 0 )); then
  say "BUILD FAILED — ${fails} failing target(s); see build-status/errors.txt"
  cat "${status_dir}/errors.txt"
  printf '{"status":"failed","steps":%s,"failures":%s,"elapsed":%s}\n' \
    "$steps" "$fails" "$elapsed" >"${status_dir}/latest.json"
  exit 1
fi

so="${src}/${out}/libweb_engine.so"
[[ -f "$so" ]] || die 'build reported success but libweb_engine.so is missing'
readonly readelf="${src}/third_party/llvm-build/Release+Asserts/bin/llvm-readelf"
"$readelf" --wide --dyn-syms "$so" 2>/dev/null \
  | grep 'OH_WindowManager_SetWindowKeepScreenOn' >/dev/null \
  || die 'OHOS screen wake lock symbol is missing from libweb_engine.so'
"$readelf" --wide --dynamic "$so" 2>/dev/null \
  | grep 'libnative_window_manager.so' >/dev/null \
  || die 'libweb_engine.so is not linked to libnative_window_manager.so'
build_id=$("$readelf" -n "$so" 2>/dev/null \
           | grep -i 'build id' | grep -oE '[0-9a-f]{40}')
say "built ${steps} step(s) in ${elapsed}s, build-id ${build_id}"

# ---- stage + package -------------------------------------------------------
if [[ -n "${SKIP_PACKAGE:-}" ]]; then
  printf '{"status":"ok","steps":%s,"elapsed":%s,"build_id":"%s","packaged":false}\n' \
    "$steps" "$elapsed" "$build_id" >"${status_dir}/latest.json"
  exit 0
fi

# hvigor rejects UNC paths outright ("Invalid project path"), and the runner
# checks the repository out inside WSL, which Windows can only reach as
# \\wsl.localhost\... So packaging runs against the working copy on the Windows
# filesystem, and the runtime has to be staged into *that* checkout. Staging
# into the workspace checkout while packaging from the other one ships whatever
# engine was staged there last: the build reports a freshly linked build-id
# while the HAP carries a stale libweb_engine.so, and nothing says otherwise.
ui_win='D:\Works\chromium-hmos\overlay\chromium-ui'
ui_wsl='/mnt/d/Works/chromium-hmos/overlay/chromium-ui'

stage_target="${repo_root}/overlay/chromium-ui"
[[ -d "$ui_wsl" ]] && stage_target="$ui_wsl"

# The app shell's own sources have to travel there too. Only the runtime was
# being staged, so hvigor compiled whatever ArkTS that checkout happened to
# hold -- for a while, a HAP whose engine was current and whose shell was
# weeks old, with nothing in the job disagreeing.
#
# Excluded: build output and oh_modules, which are the other checkout's to
# produce; the two directories stage-runtime-assets.sh is about to fill; and
# build-profile.json5 and its lock, which carry that machine's signing config
# and must not be overwritten by the repository's empty one.
if [[ "$stage_target" == "$ui_wsl" ]]; then
  say "syncing app shell sources into ${ui_wsl}"
  rsync -a --delete \
    --exclude 'build-profile.json5' \
    --exclude 'oh-package-lock.json5' \
    --exclude 'oh_modules/' \
    --exclude '.hvigor/' \
    --exclude 'entry/build/' \
    --exclude 'entry/libs/' \
    --exclude 'entry/src/main/resources/rawfile/' \
    "${repo_root}/overlay/chromium-ui/" "${ui_wsl}/" \
    || die 'app shell sync failed'
fi

say "staging runtime into ${stage_target}"
bash "${repo_root}/scripts/stage-runtime-assets.sh" "${src}/${out}" "$stage_target" \
  || die 'staging failed'

# The engine about to be packaged must be the one just linked. This is the only
# place the two can be compared before the HAP is built, and getting it wrong is
# silent in every other signal the job produces.
staged_engine="${stage_target}/entry/libs/arm64-v8a/libweb_engine.so"
staged_id=$("${src}/third_party/llvm-build/Release+Asserts/bin/llvm-readelf" -n \
            "$staged_engine" 2>/dev/null | grep -oE '[0-9a-f]{40}')
[[ "$staged_id" == "$build_id" ]] \
  || die "staged engine is ${staged_id:-missing}, expected ${build_id}"

# hvigor is a Windows toolchain; the runner lives in WSL, so the packaging step
# reaches back across the boundary. Kept last so a compile failure never gets
# this far.
say 'packaging HAP'
deveco='/mnt/d/Applications/DevEco Studio'
if [[ ! -x "${deveco}/tools/node/node.exe" ]]; then
  say 'DevEco not reachable; skipping packaging'
  printf '{"status":"ok","steps":%s,"elapsed":%s,"build_id":"%s","packaged":false}\n' \
    "$steps" "$elapsed" "$build_id" >"${status_dir}/latest.json"
  exit 0
fi
# node.exe is a Windows binary: it cannot resolve /mnt/... and silently
# rewrites such a path to \\wsl.localhost\..., which points back into WSL
# and does not exist. Its arguments must be native Windows paths.
deveco_win='D:\Applications\DevEco Studio'
if [[ ! -d "$ui_wsl" ]]; then
  say 'no Windows-side checkout for packaging; skipping'
  printf '{"status":"ok","steps":%s,"elapsed":%s,"build_id":"%s","packaged":false}\n' \
    "$steps" "$elapsed" "$build_id" >"${status_dir}/latest.json"
  exit 0
fi
# Environment variables do not cross into a Windows process on their own --
# node.exe saw DEVECO_SDK_HOME as undefined no matter how it was set on the
# command line. WSLENV is the mechanism that carries them over; /w means pass
# the value through unchanged rather than translating it as a path.
export DEVECO_SDK_HOME="${deveco_win}\\sdk"
export JAVA_HOME="${deveco_win}\\jbr"
export ComSpec='C:\Windows\System32\cmd.exe'
export WSLENV='DEVECO_SDK_HOME/w:JAVA_HOME/w:ComSpec/w:PATH/l'
# Two things have to be findable on the translated PATH.
#
# PackageHap shells out to `java`, which it looks up on PATH rather than
# through JAVA_HOME -- without it the task fails as 00308018 "Unknown Error".
#
# es2abc, the ArkTS bytecode compiler, spawns `cmd.exe` by bare name, so the
# Windows system directories have to be there too. A self-hosted runner's PATH
# does not carry them, and /l translates only what it is given. This stayed
# hidden while CompileArkTS kept hitting its up-to-date check; the first build
# that actually recompiled ArkTS failed with "10310021 ArkTS: INTERNAL ERROR
# ... spawn cmd.exe ENOENT".
export PATH="${deveco}/jbr/bin:/mnt/c/Windows/System32:/mnt/c/Windows:${PATH}"
if ( cd "$ui_wsl" \
     && "${deveco}/tools/node/node.exe" "${deveco_win}\\tools\\hvigor\\bin\\hvigorw.js" \
          --mode module -p product=default -p module=entry@default \
          assembleHap --no-daemon ) >>"$log" 2>&1
then
  :
else
  # Packaging failures were invisible to the other machine: only compiler
  # output reached errors.txt and this step simply died. Carry the log tail
  # through so a packaging break reads as clearly as a compile break.
  {
    printf '\n## packaging failed\n'
    tail -40 "$log" | grep -viE '^[[:space:]]*$'
  } >>"${status_dir}/errors.txt"
  printf '{"status":"package-failed","steps":%s,"elapsed":%s,"build_id":"%s"}\n' \
    "$steps" "$elapsed" "$build_id" >"${status_dir}/latest.json"
  die 'hvigor packaging failed (see build-status/errors.txt)'
fi

hap=$(find "${ui_wsl}/entry/build" -name '*-signed.hap' 2>/dev/null | head -1)
[[ -z "$hap" ]] && hap=$(find "${ui_wsl}/entry/build" -name '*.hap' 2>/dev/null | head -1)
[[ -n "$hap" ]] || die 'no HAP produced'
size=$(stat -c %s "$hap")

printf '{"status":"ok","steps":%s,"elapsed":%s,"build_id":"%s","packaged":true,"hap":"%s","hap_bytes":%s}\n' \
  "$steps" "$elapsed" "$build_id" "$(basename "$hap")" "$size" >"${status_dir}/latest.json"
say "HAP $(basename "$hap") ($((size / 1048576)) MB)"
printf '%s\n' "$hap" >"${status_dir}/hap-path.txt"
