#!/usr/bin/env bash
# CI-only: compile and run the production rule validator for the Linux host.
set -euo pipefail
rule_repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
rule_src=${CHROMIUM_SRC:-/root/chromium-154/src}
rule_out=${OUT_DIR:-out/ohos_arm64}
python3 -m venv "${rule_repo}/out/rule-test-env"
"${rule_repo}/out/rule-test-env/bin/python" -m pip install 'cryptography>=45,<47'
"${rule_repo}/out/rule-test-env/bin/python" "${rule_repo}/scripts/tests/test-phishing-updates.py" \
  --native-fixtures "${rule_repo}/out/rule-test-fixtures"
cd "$rule_src"
third_party/ninja/ninja -C "$rule_out" -j "${NINJA_JOBS:-12}" \
  chrome/browser:ohos_url_protection_host_tests
test_binary="${rule_out}/clang_x64/ohos_url_protection_format_tests"
if [[ ! -x "$test_binary" ]]; then
  test_binary="${rule_out}/ohos_url_protection_format_tests"
fi
"$test_binary" "${rule_repo}/out/rule-test-fixtures"
