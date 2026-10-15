#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_dir="$(mktemp -d "${TMPDIR:-/tmp}/extension-updates.XXXXXX")"
trap 'rm -rf "$test_dir"' EXIT
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
  -I "$project_root/overlay" \
  "$project_root/scripts/tests/extension-updates/result_test.cc" \
  -o "$test_dir/result-test"
"$test_dir/result-test"
