#!/usr/bin/env bash
# CI-only: compile and run the production rule validator for the Linux host.
set -euo pipefail
rule_repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
rule_src=${CHROMIUM_SRC:-/root/chromium-154/src}
rule_out=${OUT_DIR:-out/ohos_arm64}
# Deterministic, Python-signed vectors are versioned with the tests. The
# publication job verifies they still match the signing tool; the native
# runner does not need a Python package manager or production signing key.
cd "$rule_src"
third_party/ninja/ninja -C "$rule_out" -j "${NINJA_JOBS:-12}" \
  chrome/browser:ohos_url_protection_host_tests
test_binary="${rule_out}/clang_x64/ohos_url_protection_format_tests"
if [[ ! -x "$test_binary" ]]; then
  test_binary="${rule_out}/ohos_url_protection_format_tests"
fi
cp "$rule_out/icudtl.dat" "$(dirname "$test_binary")/icudtl.dat"
benchmark_dir="${RUNNER_TEMP:-/tmp}/rule-source-benchmarks"
python3 "${rule_repo}/scripts/tests/fetch-rule-source-benchmarks.py" "$benchmark_dir"
"$test_binary" "${rule_repo}/scripts/tests/fixtures/phishing-updates" "$benchmark_dir"
