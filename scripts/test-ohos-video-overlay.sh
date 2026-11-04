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
"${CXX:-c++}" -std=c++17 -pthread -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$overlay_repo/overlay" \
  "$overlay_repo/scripts/tests/ohos-video-overlay/marker_thread_test.cc" \
  -o "$overlay_tests/marker-thread"
"$overlay_tests/marker-thread"
host_flags=()
if [[ "$(uname -s)" == Darwin ]]; then
  # Darwin socketpair lacks these Linux/OHOS creation flags. No blocking I/O
  # or exec is used by this host-channel test; production uses the NDK flags.
  host_flags=(-DSOCK_CLOEXEC=0 -DSOCK_NONBLOCK=0)
fi
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "${host_flags[@]}" -I"$overlay_repo/overlay" \
  "$overlay_repo/scripts/tests/ohos-video-overlay/host_channel_test.cc" \
  -o "$overlay_tests/host-channel"
"$overlay_tests/host-channel"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$overlay_repo/overlay" \
  "$overlay_repo/scripts/tests/ohos-video-overlay/transport_test.cc" \
  "$overlay_repo/overlay/ui/ozone/platform/ohos/ohos_overlay_transport.cc" \
  -o "$overlay_tests/transport"
"$overlay_tests/transport"
python3 "$overlay_repo/scripts/tests/test-software-video-overlay.py"
