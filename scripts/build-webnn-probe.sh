#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
native_sdk="${OHOS_NATIVE_SDK:-/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native}"
probe_out="${1:-${repo_root}/artifacts/webnn-research/nnrt-probe}"
mkdir -p "$(dirname "$probe_out")"
"${native_sdk}/llvm/bin/clang++" --target=aarch64-linux-ohos \
  --sysroot="${native_sdk}/sysroot" -std=c++17 -static-libstdc++ -O2 -Wall -Wextra -Werror \
  -I"${repo_root}/overlay" \
  "${repo_root}/overlay/services/webnn/ohos/nnrt_add_model.cc" \
  "${repo_root}/tools/webnn/nnrt-probe.cc" \
  -lneural_network_runtime -lneural_network_core -o "$probe_out"
printf 'Built (not executed): %s\n' "$probe_out"
