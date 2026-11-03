#!/usr/bin/env bash
set -euo pipefail
# Standalone NDK bridge: no Chromium __Cr / system __h C++ object mixing.
root="$(cd "$(dirname "$0")/.." && pwd)"
sdk="${OHOS_NATIVE_SDK:-/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native}"
output="${1:-$root/artifacts/ohos-overlay/libohos_video_overlay.so}"
mkdir -p "$(dirname "$output")"
source_dir="$root/overlay/ui/ozone/platform/ohos"
"$sdk/llvm/bin/clang++" --target=aarch64-linux-ohos --sysroot="$sdk/sysroot" \
  -std=c++17 -O2 -fno-exceptions -fno-rtti -D_LIBCPP_ABI_NAMESPACE=__h \
  -Wall -Wextra -Werror -fPIC -fvisibility=hidden -shared \
  "$source_dir/ohos_overlay_session.cc" "$source_dir/ohos_rs_overlay_backend.cc" \
  "$source_dir/ohos_overlay_bridge.cc" "$source_dir/ohos_overlay_transport.cc" \
  "$source_dir/ohos_overlay_host.cc" -lnative_buffer -lipc_capi -lace_ndk.z -luv -ldl \
  -Wl,--no-undefined -Wl,-soname,libohos_video_overlay.so -o "$output"
printf '%s\n' "$output"
