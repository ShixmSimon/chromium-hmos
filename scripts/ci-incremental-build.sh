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
#   SKIP_HAR=1     scripts/ci-incremental-build.sh   # HAP but no engine HAR
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
  local rel="${patch#"${repo_root}/"}"
  if git -C "$src" apply --check "$patch" 2>/dev/null; then
    git -C "$src" apply "$patch" || die "failed to apply ${name}"
    say "applied incremental patch ${name}"
    return
  fi
  # The patch was edited since this tree last built. The tree is persistent and
  # never reset, so an older revision of this same patch may still be applied
  # to it. Back that revision out and apply the new one. Without this, editing
  # any patch in the list wedges every build after it with "neither applies
  # nor is already present".
  #
  # "Reverse-applies" alone cannot tell which revision is in the tree: an edit
  # that only removes a line leaves the new revision's context intact, so it
  # reverse-applies over the old one too (ohos-screen-orientation.patch
  # dropped a `deps +=` line and was reported as already applied while the
  # old line kept breaking gn). Of the revisions that reverse-apply, the one
  # adding the most lines is the one actually present.
  local rev best_rev="" best_lines=-1 lines
  if git -C "$src" apply --reverse --check "$patch" 2>/dev/null; then
    best_rev=current
    best_lines=$(grep -c '^+[^+]' "$patch" || true)
  fi
  for rev in $(git -C "$repo_root" log --format=%H -n 20 -- "$rel"); do
    if git -C "$repo_root" show "${rev}:${rel}" 2>/dev/null | cmp -s - "$patch"; then
      continue
    fi
    if git -C "$repo_root" show "${rev}:${rel}" 2>/dev/null |
        git -C "$src" apply --reverse --check - 2>/dev/null; then
      lines=$(git -C "$repo_root" show "${rev}:${rel}" | grep -c '^+[^+]' || true)
      if (( lines > best_lines )); then
        best_rev=$rev
        best_lines=$lines
      fi
    fi
  done
  if [[ "$best_rev" == current ]]; then
    say "incremental patch ${name} already applied"
    return
  fi
  if [[ -n "$best_rev" ]]; then
    git -C "$repo_root" show "${best_rev}:${rel}" | git -C "$src" apply --reverse ||
      die "failed to back out ${name} at ${best_rev}"
    say "backed out incremental patch ${name} at ${best_rev:0:8}"
    git -C "$src" apply "$patch" || die "failed to apply ${name}"
    say "applied incremental patch ${name}"
    return
  fi
  git -C "$src" apply --check --verbose "$patch" || true
  die "incremental patch ${name} neither applies nor is already present"
}

# The counterpart to the above: a patch that is no longer wanted. The tree is
# persistent, so dropping the apply line is not enough -- the hunks stay in it
# forever. This backs out whichever revision of the patch is actually applied,
# and says nothing if none is. Safe to leave in place for a build or two after
# the patch file is deleted, since it reads the file out of history.
retire_incremental_patch() {
  local patch="$1"
  local name="${patch##*/}"
  local rel="${patch#"${repo_root}/"}"
  local rev
  for rev in $(git -C "$repo_root" log --format=%H -n 20 -- "$rel"); do
    if git -C "$repo_root" show "${rev}:${rel}" 2>/dev/null |
        git -C "$src" apply --reverse --check - 2>/dev/null; then
      git -C "$repo_root" show "${rev}:${rel}" | git -C "$src" apply --reverse ||
        die "failed to retire ${name}"
      say "retired incremental patch ${name} at ${rev:0:8}"
      return
    fi
  done
  say "incremental patch ${name} is not applied"
}

apply_incremental_patch "${repo_root}/patches/ohos-audio-input.patch"
apply_incremental_patch "${repo_root}/patches/ohos-vibration.patch"
apply_incremental_patch "${repo_root}/patches/ohos-battery.patch"
apply_incremental_patch "${repo_root}/patches/ohos-wake-lock.patch"
apply_incremental_patch "${repo_root}/patches/ohos-web-bluetooth.patch"
apply_incremental_patch "${repo_root}/patches/ohos-web-bluetooth-api.patch"
apply_incremental_patch "${repo_root}/patches/ohos-web-usb.patch"
apply_incremental_patch "${repo_root}/patches/ohos-web-hid.patch"
apply_incremental_patch "${repo_root}/patches/ohos-web-serial.patch"
apply_incremental_patch "${repo_root}/patches/ohos-display-capture.patch"
# The press no longer steals focus from an open bubble, so the chooser does
# not need its own opt-out from close-on-deactivate.
retire_incremental_patch "${repo_root}/patches/ohos-web-bluetooth-chooser-input.patch"
apply_incremental_patch "${repo_root}/patches/ohos-device-chooser-width.patch"
apply_incremental_patch "${repo_root}/patches/ohos-dialog-width-fits-screen.patch"
apply_incremental_patch "${repo_root}/patches/ohos-bubble-anchor-top-center.patch"
apply_incremental_patch "${repo_root}/patches/ohos-browser-controls.patch"
apply_incremental_patch "${repo_root}/patches/ohos-tts.patch"
apply_incremental_patch "${repo_root}/patches/ohos-notifications.patch"
apply_incremental_patch "${repo_root}/patches/ohos-media-session.patch"
apply_incremental_patch "${repo_root}/patches/ohos-screen-orientation.patch"
apply_incremental_patch "${repo_root}/patches/ohos-shape-detection.patch"
apply_incremental_patch "${repo_root}/patches/ohos-webauthn-platform.patch"
apply_incremental_patch "${repo_root}/patches/ohos-video-codec.patch"
# The chooser takes touch now, so the probes that found out why come out.
retire_incremental_patch "${repo_root}/patches/ohos-bubble-input-diagnostics.patch"

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
    printf '\n## log tail\n'
    tr '\r' '\n' <"$log" | tail -120
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
# produce; the two directories stage-runtime-assets.sh is about to fill; the
# root build-profile.json5, which carries that machine's signing config and
# must not be overwritten by the repository's empty one; and the ohpm lock
# files, which ohpm install regenerates there.
#
# The build-profile.json5 exclude is anchored with a leading slash on purpose.
# Unanchored, rsync matched the file at every depth: every module's own
# build-profile.json5 was held back too, so entry's never reached that checkout
# and the engine HAR arrived without one -- hvigor then failed with "Can not
# find build config file build-profile.json5 at 'engine'".
if [[ "$stage_target" == "$ui_wsl" ]]; then
  say "syncing app shell sources into ${ui_wsl}"
  rsync -a --delete \
    --exclude '/build-profile.json5' \
    --exclude 'oh-package-lock.json5' \
    --exclude 'oh_modules/' \
    --exclude '.hvigor/' \
    --exclude 'entry/build/' \
    --exclude 'entry/libs/' \
    --exclude 'entry/src/main/resources/rawfile/' \
    --exclude 'engine/build/' \
    --exclude 'engine/libs/' \
    --exclude 'engine/src/main/resources/rawfile/' \
    "${repo_root}/overlay/chromium-ui/" "${ui_wsl}/" \
    || die 'app shell sync failed'

  # The engine moved into the engine HAR. Its old home in the entry module is
  # excluded from the sync above, so nothing would ever clear it: left there,
  # the HAP would carry a second, stale libweb_engine.so beside the new one,
  # and the build-id check below -- which reads the staging directory -- would
  # pass while the package shipped the old engine. Idempotent.
  rm -rf "${ui_wsl}/entry/libs" "${ui_wsl}/entry/src/main/resources/rawfile/chromium"
fi

say "staging runtime into ${stage_target}"
bash "${repo_root}/scripts/stage-runtime-assets.sh" "${src}/${out}" "$stage_target" \
  || die 'staging failed'

# The engine about to be packaged must be the one just linked. This is the only
# place the two can be compared before the HAP is built, and getting it wrong is
# silent in every other signal the job produces.
staged_engine="${stage_target}/engine/libs/arm64-v8a/libweb_engine.so"
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

# That checkout's build-profile.json5 is its own -- it carries the signing
# config and is excluded from the sync -- so a module added to the repository's
# copy does not reach it. hvigor would then fail on the dependency with an
# error that does not say this. Say it instead.
if ! grep -Eq '"srcPath"[[:space:]]*:[[:space:]]*"\./engine"' "${ui_wsl}/build-profile.json5"; then
  {
    printf '\n## packaging failed\n'
    printf '%s/build-profile.json5 does not register the engine module.\n' "$ui_win"
    printf 'Add this to its "modules" array, after the entry module:\n'
    printf '    {\n      "name": "engine",\n      "srcPath": "./engine"\n    }\n'
  } >>"${status_dir}/errors.txt"
  printf '{"status":"package-failed","steps":%s,"elapsed":%s,"build_id":"%s"}\n' \
    "$steps" "$elapsed" "$build_id" >"${status_dir}/latest.json"
  die "engine module not registered in ${ui_win}\\build-profile.json5"
fi

# entry depends on the engine HAR as a local file dependency; ohpm links it into
# oh_modules. oh_modules and the lock file are excluded from the sync, so this
# runs on that checkout every time.
ohpm_cli="${deveco}/tools/ohpm/bin/pm-cli.js"
[[ -f "$ohpm_cli" ]] || die "ohpm not found at ${ohpm_cli}"
( cd "$ui_wsl" \
  && "${deveco}/tools/node/node.exe" "${deveco_win}\\tools\\ohpm\\bin\\pm-cli.js" install ) \
  >>"$log" 2>&1 || die 'ohpm install failed'

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

# The check that matters is on the package, not on the staging directory: that
# the libweb_engine.so inside the HAP is the one just linked.
packaged_engine="$(mktemp)"
if command -v unzip >/dev/null; then
  unzip -p "$hap" libs/arm64-v8a/libweb_engine.so >"$packaged_engine" 2>/dev/null
else
  python3 -c 'import sys,zipfile; sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read("libs/arm64-v8a/libweb_engine.so"))' \
    "$hap" >"$packaged_engine" 2>/dev/null
fi || die "HAP has no libs/arm64-v8a/libweb_engine.so"
packaged_id=$("${src}/third_party/llvm-build/Release+Asserts/bin/llvm-readelf" -n \
              "$packaged_engine" 2>/dev/null | grep -oE '[0-9a-f]{40}')
rm -f "$packaged_engine"
[[ "$packaged_id" == "$build_id" ]] \
  || die "HAP carries engine ${packaged_id:-with no build-id}, expected ${build_id}"

# The engine HAR is the other deliverable of the split: a shell that draws
# only UI imports this and gets the engine, its runtime and its ArkTS bridges.
# assembleHap already builds the engine module as entry's dependency, so this
# only packages it -- and it runs after the HAP so a HAR problem cannot cost a
# package that was already good.
har=''
har_size=0
if [[ -z "${SKIP_HAR:-}" ]]; then
  say 'packaging engine HAR'
  if ( cd "$ui_wsl" \
       && "${deveco}/tools/node/node.exe" "${deveco_win}\\tools\\hvigor\\bin\\hvigorw.js" \
            --mode module -p product=default -p module=engine@default \
            assembleHar --no-daemon ) >>"$log" 2>&1
  then
    har=$(find "${ui_wsl}/engine/build" -name '*.har' 2>/dev/null | head -1)
  fi
  if [[ -z "$har" ]]; then
    {
      printf '\n## engine HAR not produced\n'
      tail -40 "$log" | grep -viE '^[[:space:]]*$'
    } >>"${status_dir}/errors.txt"
    die 'assembleHar produced no .har (see build-status/errors.txt)'
  fi

  # A HAR is a gzipped tar rooted at package/, not a zip -- unzip calls it
  # "not a zip file". Same check the HAP gets: the engine inside the artifact
  # has to be the one this run linked.
  har_engine="$(mktemp)"
  tar -xzOf "$har" package/libs/arm64-v8a/libweb_engine.so >"$har_engine" 2>/dev/null \
    || die 'HAR has no package/libs/arm64-v8a/libweb_engine.so'
  har_id=$("${src}/third_party/llvm-build/Release+Asserts/bin/llvm-readelf" -n \
           "$har_engine" 2>/dev/null | grep -oE '[0-9a-f]{40}')
  rm -f "$har_engine"
  [[ "$har_id" == "$build_id" ]] \
    || die "HAR carries engine ${har_id:-with no build-id}, expected ${build_id}"

  har_size=$(stat -c %s "$har")
  say "HAR $(basename "$har") ($((har_size / 1048576)) MB)"
  printf '%s\n' "$har" >"${status_dir}/har-path.txt"
fi

printf '{"status":"ok","steps":%s,"elapsed":%s,"build_id":"%s","packaged":true,"hap":"%s","hap_bytes":%s,"har":"%s","har_bytes":%s}\n' \
  "$steps" "$elapsed" "$build_id" "$(basename "$hap")" "$size" \
  "${har:+$(basename "$har")}" "$har_size" >"${status_dir}/latest.json"
say "HAP $(basename "$hap") ($((size / 1048576)) MB)"
printf '%s\n' "$hap" >"${status_dir}/hap-path.txt"
