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
#   patches/  are NOT re-applied. Doing that safely means resetting the tree to
#             pristine and rebuilding everything, so it is a separate, explicit
#             operation (FULL=1) rather than something a push triggers by
#             accident.
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
    chromium-ui/*|arkweb/*|ohos_arkweb_playground/*) continue ;;  # app side, not Chromium
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
build_id=$("${src}/third_party/llvm-build/Release+Asserts/bin/llvm-readelf" -n "$so" 2>/dev/null \
           | grep -i 'build id' | grep -oE '[0-9a-f]{40}')
say "built ${steps} step(s) in ${elapsed}s, build-id ${build_id}"

# ---- stage + package -------------------------------------------------------
if [[ -n "${SKIP_PACKAGE:-}" ]]; then
  printf '{"status":"ok","steps":%s,"elapsed":%s,"build_id":"%s","packaged":false}\n' \
    "$steps" "$elapsed" "$build_id" >"${status_dir}/latest.json"
  exit 0
fi

say 'staging runtime'
bash "${repo_root}/scripts/stage-runtime-assets.sh" "${src}/${out}" || die 'staging failed'

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
( cd "${repo_root}/overlay/chromium-ui" \
  && DEVECO_SDK_HOME="D:\\Applications\\DevEco Studio\\sdk" \
     JAVA_HOME="D:\\Applications\\DevEco Studio\\jbr" \
     "${deveco}/tools/node/node.exe" "${deveco}/tools/hvigor/bin/hvigorw.js" \
       --mode module -p product=default -p module=entry@default \
       assembleHap --no-daemon ) >>"$log" 2>&1 \
  || die 'hvigor packaging failed'

hap=$(find "${repo_root}/overlay/chromium-ui/entry/build" -name '*-signed.hap' 2>/dev/null | head -1)
[[ -z "$hap" ]] && hap=$(find "${repo_root}/overlay/chromium-ui/entry/build" -name '*.hap' 2>/dev/null | head -1)
[[ -n "$hap" ]] || die 'no HAP produced'
size=$(stat -c %s "$hap")

printf '{"status":"ok","steps":%s,"elapsed":%s,"build_id":"%s","packaged":true,"hap":"%s","hap_bytes":%s}\n' \
  "$steps" "$elapsed" "$build_id" "$(basename "$hap")" "$size" >"${status_dir}/latest.json"
say "HAP $(basename "$hap") ($((size / 1048576)) MB)"
printf '%s\n' "$hap" >"${status_dir}/hap-path.txt"
