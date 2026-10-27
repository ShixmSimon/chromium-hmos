#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_dir="$(mktemp -d "${TMPDIR:-/tmp}/jit-startup.XXXXXX")"
trap 'rm -rf "$test_dir"' EXIT
"${CXX:-clang++}" -std=c++20 -Wall -Wextra -Werror -Wunreachable-code-aggressive \
  -I "$project_root/overlay" \
  "$project_root/scripts/tests/jit-startup/startup_test.cc" \
  -o "$test_dir/startup-test"
"$test_dir/startup-test"
