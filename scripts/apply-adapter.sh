#!/usr/bin/env bash

set -euo pipefail

readonly expected_revision="f405107495a07cb1bfcf687d4af8d91117098db6"
readonly project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly chromium_src="${1:-}"

if [[ -z "${chromium_src}" || ! -d "${chromium_src}/.git" ]]; then
  echo "usage: $0 /path/to/chromium/src" >&2
  exit 2
fi

actual_revision="$(git -C "${chromium_src}" rev-parse HEAD)"
if [[ "${actual_revision}" != "${expected_revision}" ]]; then
  echo "expected Chromium ${expected_revision}, got ${actual_revision}" >&2
  exit 1
fi

if [[ -n "$(git -C "${chromium_src}" status --porcelain)" ]]; then
  echo "Chromium checkout must be clean before applying the adapter." >&2
  exit 1
fi

# DEPS repositories are separate checkouts; their OHOS changes are listed in
# patches/deps/series as "<path relative to src> <patch file>".
readonly deps_patch_root="${project_root}/patches/deps"
while read -r deps_path deps_patch; do
  [[ -z "${deps_path}" || "${deps_path}" == \#* ]] && continue
  if [[ -n "$(git -C "${chromium_src}/${deps_path}" status --porcelain)" ]]; then
    echo "${deps_path} must be clean before applying the adapter." >&2
    exit 1
  fi
done <"${deps_patch_root}/series"

git -C "${chromium_src}" apply --binary \
  "${project_root}/patches/chromium-150-harmonyos.patch"
while read -r deps_path deps_patch; do
  [[ -z "${deps_path}" || "${deps_path}" == \#* ]] && continue
  git -C "${chromium_src}/${deps_path}" apply --binary \
    "${deps_patch_root}/${deps_patch}"
done <"${deps_patch_root}/series"
rsync -a "${project_root}/overlay/" "${chromium_src}/"

deps_root="$(dirname "${chromium_src}")/deps_code/webview"
mkdir -p "${deps_root}"
rsync -a "${project_root}/external/deps_code/webview/" "${deps_root}/"

echo "Adapter applied. Configure src/ohos_sdk locally before gn gen."

