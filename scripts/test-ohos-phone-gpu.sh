#!/usr/bin/env bash
set -euo pipefail
gpu_repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
gpu_tmp=$(mktemp -d)
trap 'rm -rf "$gpu_tmp"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
  -I "$gpu_repo/overlay" \
  "$gpu_repo/overlay/ui/ozone/platform/ohos/ohos_phone_gpu_transport.cc" \
  "$gpu_repo/scripts/tests/ohos-phone-gpu/transport_test.cc" \
  -Wl,--wrap=fstat -o "$gpu_tmp/transport-test"
timeout 15s "$gpu_tmp/transport-test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
  -I "$gpu_repo/overlay" \
  "$gpu_repo/overlay/ui/ozone/platform/ohos/ohos_phone_gpu_shared_transport.cc" \
  "$gpu_repo/scripts/tests/ohos-phone-gpu/shared_transport_test.cc" \
  -o "$gpu_tmp/shared-transport-test"
timeout 15s "$gpu_tmp/shared-transport-test"
