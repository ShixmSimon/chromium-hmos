#!/usr/bin/env bash
# Compile the production libc/STL transport, independent of a Chromium checkout.
set -euo pipefail
fork_repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
fork_tests="$fork_repo/scripts/tests/ohos-fork-server"
fork_tmp=$(mktemp -d)
trap 'rm -rf "$fork_tmp"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fPIC -shared -pthread \
  -I "$fork_repo/overlay" -I "$fork_tests/stubs" "$fork_tests/child.cc" -o "$fork_tmp/libnweb_render.so"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread -rdynamic \
  -I "$fork_repo/overlay" -I "$fork_tests/stubs" \
  "$fork_repo/overlay/base/process/fork_server_ohos.cc" "$fork_tests/suite.cc" \
  -ldl -o "$fork_tmp/fork-server-tests"
timeout 60s "$fork_tmp/fork-server-tests" "$fork_tmp/libnweb_render.so"
timeout 15s "$fork_tmp/fork-server-tests" "$fork_tmp/libnweb_render.so" --server-loss
timeout 15s "$fork_tmp/fork-server-tests" "$fork_tmp/libnweb_render.so" --missing-library
timeout 15s "$fork_tmp/fork-server-tests" "$fork_tmp/libnweb_render.so" --loader-fds
