#!/usr/bin/env bash
# Resolve phone crash PCs against the runner's existing binary before rebuilding.
set -euo pipefail
crash_src="${CHROMIUM_SRC:-/root/chromium-154/src}"
crash_out="${OUT_DIR:-out/ohos_arm64}"
crash_elf="${crash_src}/${crash_out}/exe.unstripped/libweb_engine.so"
crash_tools="${crash_src}/third_party/llvm-build/Release+Asserts/bin"
[[ -n "${CRASH_BUILD_ID:-}" && -n "${CRASH_ADDRESSES:-}" ]] || exit 0
[[ "$CRASH_BUILD_ID" =~ ^[0-9a-fA-F]{32,64}$ ]] || { echo 'Invalid build ID'; exit 1; }
read -r -a crash_addresses <<< "$CRASH_ADDRESSES"
for crash_pc in "${crash_addresses[@]}"; do
  [[ "$crash_pc" =~ ^0x[0-9a-fA-F]{1,16}$ ]] || { echo 'Invalid crash address'; exit 1; }
done
if [[ ! -f "$crash_elf" ]]; then
  echo 'Previous unstripped binary is unavailable; skipping symbolization.'
  exit 0
fi
crash_actual_id=$("${crash_tools}/llvm-readelf" -n "$crash_elf" | awk '/Build ID:/ {print $3; exit}')
if [[ "$crash_actual_id" != "$CRASH_BUILD_ID" ]]; then
  echo "Previous build ID $crash_actual_id differs from requested $CRASH_BUILD_ID; skipping."
  exit 0
fi
printf 'Verified native build ID: %s\n' "$crash_actual_id"
"${crash_tools}/llvm-symbolizer" --obj="$crash_elf" --functions=linkage --inlines \
  --print-address "${crash_addresses[@]}"
