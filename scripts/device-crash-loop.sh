#!/usr/bin/env bash
# Copyright (c) 2026
# Licensed under the Apache License, Version 2.0.
#
# Install the adapter on an attached device, launch it, and if it dies, print a
# symbolized stack.
#
# Bringing up this port is a loop: launch, crash, symbolize, fix one guard,
# rebuild. The slow part should be the rebuild, not reassembling the diagnosis
# by hand each time. Notably the faultlog directory is unreadable on a
# production device, but processdump also writes the frames to hilog, which is
# readable -- that is where the stack comes from.
#
# The shipped library is stripped; exe.unstripped/libweb_engine.so carries the
# same build id, so offsets resolve exactly. The script checks that, because
# symbolizing against a mismatched build silently produces plausible nonsense.
#
# Usage:
#   scripts/device-crash-loop.sh [HAP] [OUT_DIR]

set -uo pipefail

HAP=${1:-overlay/chromium-ui/entry/build/default/outputs/default/entry-default-signed.hap}
OUT=${2:-/root/chromium-154/src/out/ohos_arm64}
BUNDLE=com.caidingding233.chromium
ABILITY=EntryAbility
HDC=${HDC:-hdc}
SYMBOLIZER="${OUT}/../../third_party/llvm-build/Release+Asserts/bin/llvm-symbolizer"
READELF="${OUT}/../../third_party/llvm-build/Release+Asserts/bin/llvm-readelf"

die() { printf '%s\n' "$*" >&2; exit 1; }

command -v "$HDC" >/dev/null || die "hdc not on PATH (set HDC=...)"
"$HDC" list targets | grep -q . || die 'no device attached'
[[ -f "$HAP" ]] || die "no HAP at $HAP"
[[ -f "${OUT}/exe.unstripped/libweb_engine.so" ]] \
  || die "no unstripped library at ${OUT}/exe.unstripped/libweb_engine.so"

echo '== installing =='
"$HDC" shell "aa force-stop ${BUNDLE}" >/dev/null 2>&1
"$HDC" install -r "$HAP" 2>&1 | tail -1

echo '== launching =='
"$HDC" shell 'hilog -r' >/dev/null 2>&1
"$HDC" shell "power-shell wakeup" >/dev/null 2>&1
out=$("$HDC" shell "aa start -a ${ABILITY} -b ${BUNDLE}" 2>&1)
printf '   %s\n' "$out"
case "$out" in
  *"screen is locked"*) die 'device is locked -- unlock it and re-run' ;;
esac

for i in $(seq 1 12); do
  sleep 2
  pid=$("$HDC" shell "pidof ${BUNDLE}" 2>/dev/null | tr -d '\r')
  [[ -z "$pid" ]] && break
done

if [[ -n "${pid:-}" ]]; then
  echo "== still running after ~24s (pid ${pid}) =="
  echo '   no crash. Check the UI for diagnostic toasts.'
  exit 0
fi

echo '== process gone, collecting crash =='
"$HDC" shell 'hilog -x' 2>/dev/null | tr -d '\r' > /tmp/crash-hilog.txt

reason=$(grep -m1 'Reason:Signal' /tmp/crash-hilog.txt)
thread=$(grep -m1 'DfxFaultLogger: Tid:' /tmp/crash-hilog.txt)
[[ -z "$reason" ]] && {
  echo '   no crash signature in hilog -- the process may have exited cleanly.'
  echo '   last app log lines:'
  grep -E "${BUNDLE}" /tmp/crash-hilog.txt | tail -20
  exit 0
}
printf '   %s\n   %s\n\n' "${reason#*DfxFaultLogger: }" "${thread#*DfxFaultLogger: }"

# The build id in the frames must match the library we symbolize against.
frame_build_id=$(grep -m1 -oE 'libweb_engine\.so\(([0-9a-f]+)\)' /tmp/crash-hilog.txt \
                 | grep -oE '[0-9a-f]{40}')
local_build_id=$("$READELF" -n "${OUT}/exe.unstripped/libweb_engine.so" 2>/dev/null \
                 | grep -i 'build id' | grep -oE '[0-9a-f]{40}')
if [[ -n "$frame_build_id" && "$frame_build_id" != "$local_build_id" ]]; then
  echo "   WARNING: device build id ${frame_build_id}"
  echo "            local  build id ${local_build_id}"
  echo '            The device is running a different build. Symbols below are wrong.'
  echo
fi

echo '== stack =='
grep -E 'DfxFaultLogger: #[0-9]+ pc' /tmp/crash-hilog.txt | while read -r line; do
  off=$(awk '{print $NF}' <<<"${line%% /data/*}")
  off=$(grep -oE '#[0-9]+ pc [0-9a-f]+' <<<"$line" | awk '{print $3}')
  frame=$(grep -oE '#[0-9]+' <<<"$line" | head -1)
  if grep -q 'libweb_engine.so' <<<"$line"; then
    sym=$("$SYMBOLIZER" --obj="${OUT}/exe.unstripped/libweb_engine.so" \
                        --demangle --inlines "0x${off}" 2>/dev/null | head -2 | paste -sd' | ')
    printf '   %s  %s\n' "$frame" "${sym:-<unresolved>}"
  else
    printf '   %s  %s\n' "$frame" "$(sed 's/.*pc [0-9a-f]* //' <<<"$line")"
  fi
done

echo
echo 'full hilog: /tmp/crash-hilog.txt'
