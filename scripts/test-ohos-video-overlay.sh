#!/usr/bin/env bash
set -euo pipefail
overlay_repo="$(cd "$(dirname "$0")/.." && pwd)"
overlay_tests="$(mktemp -d)"
trap 'rm -rf "$overlay_tests"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$overlay_repo/overlay" \
  "$overlay_repo/scripts/tests/ohos-video-overlay/session_test.cc" \
  "$overlay_repo/overlay/ui/ozone/platform/ohos/ohos_overlay_session.cc" \
  -o "$overlay_tests/session"
"$overlay_tests/session"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$overlay_repo/overlay" \
  "$overlay_repo/scripts/tests/ohos-video-overlay/geometry_test.cc" \
  -o "$overlay_tests/geometry"
"$overlay_tests/geometry"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$overlay_repo/overlay" \
  "$overlay_repo/scripts/tests/ohos-video-overlay/rs_abi_test.cc" \
  -o "$overlay_tests/rs-abi"
"$overlay_tests/rs-abi"
