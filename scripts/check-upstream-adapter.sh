#!/usr/bin/env bash

set -euo pipefail

readonly project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly version="${1:?usage: $0 VERSION REVISION}"
readonly revision="${2:?usage: $0 VERSION REVISION}"
readonly work_root="$(mktemp -d "${RUNNER_TEMP:-/tmp}/chromium-adapter-check.XXXXXX")"
readonly checkout="${work_root}/src"
readonly report="${project_root}/upstream-compatibility.md"

cleanup() {
  rm -rf "${work_root}"
}
trap cleanup EXIT

git init -q "${checkout}"
git -C "${checkout}" remote add origin https://github.com/chromium/chromium.git
git -C "${checkout}" sparse-checkout init --no-cone
grep '^diff --git ' "${project_root}/patches/chromium-154-harmonyos.patch" |
  awk '{sub(/^b\//, "/", $4); print $4}' |
  LC_ALL=C sort -u >"${checkout}/.git/info/sparse-checkout"
git -C "${checkout}" fetch -q --depth=1 --filter=blob:none origin "${revision}"
git -C "${checkout}" checkout -q --detach FETCH_HEAD

compatible=true
failure_log="${work_root}/apply-error.log"
if ! git -C "${checkout}" apply --check --binary \
  "${project_root}/patches/chromium-154-harmonyos.patch" 2>"${failure_log}"; then
  compatible=false
fi

if [[ "${compatible}" == true ]]; then
  git -C "${checkout}" apply --binary \
    "${project_root}/patches/chromium-154-harmonyos.patch"
  rsync -a "${project_root}/overlay/" "${checkout}/"
  if [[ ! -f "${checkout}/chromium-ui/entry/src/main/ets/pages/AuraShell.ets" ||
        ! -f "${checkout}/ohos_nweb/src/aura_shell/ohos_chrome_main_runner.cc" ||
        ! -L "${checkout}/ohos_nweb" ]]; then
    compatible=false
    printf 'Adapter overlay validation failed.\n' >"${failure_log}"
  fi
fi

{
  printf '# Chromium upstream compatibility\n\n'
  printf -- '- Version: `%s`\n' "${version}"
  printf -- '- Revision: `%s`\n' "${revision}"
  printf -- '- Existing adapter applies cleanly: `%s`\n' "${compatible}"
  if [[ "${compatible}" != true ]]; then
    printf '\n```text\n'
    sed -n '1,160p' "${failure_log}"
    printf '```\n'
  fi
} >"${report}"

printf 'Adapter compatibility for Chromium %s: %s\n' "${version}" "${compatible}"
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
  printf 'compatible=%s\n' "${compatible}" >>"${GITHUB_OUTPUT}"
fi

if [[ "${REQUIRE_COMPATIBLE:-0}" == 1 && "${compatible}" != true ]]; then
  exit 1
fi
