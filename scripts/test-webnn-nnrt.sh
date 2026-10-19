#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
native_sdk="${OHOS_NATIVE_SDK:-/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native}"
test_dir="$(mktemp -d "${TMPDIR:-/tmp}/webnn-nnrt.XXXXXX")"
trap 'rm -rf "$test_dir"' EXIT
mkdir -p "$test_dir/include"
ln -s "${native_sdk}/sysroot/usr/include/neural_network_runtime" "$test_dir/include/neural_network_runtime"
ln -s "${native_sdk}/sysroot/usr/include/info" "$test_dir/include/info"
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -Wno-ignored-attributes \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"${repo_root}/overlay" \
  -I"$test_dir/include" \
  "${repo_root}/overlay/services/webnn/ohos/nnrt_add_model.cc" \
  "${repo_root}/scripts/tests/webnn-nnrt-test.cc" -o "$test_dir/test"
"$test_dir/test"
