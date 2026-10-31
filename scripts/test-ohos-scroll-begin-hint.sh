#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_dir="$(mktemp -d "${TMPDIR:-/tmp}/ohos-scroll-hint.XXXXXX")"
trap 'rm -rf "$test_dir"' EXIT
"${CXX:-clang++}" -std=c++20 -Wall -Wextra -Werror \
  -I "$project_root/overlay" \
  "$project_root/scripts/tests/ohos-scroll-begin-hint_test.cc" \
  -o "$test_dir/scroll-hint-test"
"$test_dir/scroll-hint-test"
