#!/usr/bin/env bash

set -euo pipefail

readonly expected_revision="b510e9d7cd3a2fbd78d0ddc42234103206c5f78d"  # 154.0.8037.97
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
  "${project_root}/patches/chromium-154-harmonyos.patch"
git -C "${chromium_src}" apply \
  "${project_root}/patches/ohos-url-protection.patch"
git -C "${chromium_src}" apply \
  "${project_root}/patches/ohos-url-protection-updates.patch"
git -C "${chromium_src}" apply \
  "${project_root}/patches/ohos-rule-sources.patch"
git -C "${chromium_src}" apply \
  "${project_root}/patches/ohos-phone-fork.patch"
git -C "${chromium_src}" apply \
  "${project_root}/patches/ohos-phone-gpu-shared-output.patch"
git -C "${chromium_src}" apply \
  "${project_root}/patches/ohos-ime-explicit-keyboard-request.patch"
git -C "${chromium_src}" apply \
  "${project_root}/patches/ohos-phone-skia-vulkan.patch"
git -C "${chromium_src}" apply \
  "${project_root}/patches/ohos-skia-output-format-diagnostics.patch"
git -C "${chromium_src}" apply \
  "${project_root}/patches/ohos-vulkan-sdr-output.patch"
while read -r deps_path deps_patch; do
  [[ -z "${deps_path}" || "${deps_path}" == \#* ]] && continue
  git -C "${chromium_src}/${deps_path}" apply --binary \
    "${deps_patch_root}/${deps_patch}"
done <"${deps_patch_root}/series"
git -C "${chromium_src}/third_party/angle" apply \
  "${project_root}/patches/ohos-angle-vulkan-msrt.patch"
rsync -a "${project_root}/overlay/" "${chromium_src}/"

deps_root="$(dirname "${chromium_src}")/deps_code/webview"
mkdir -p "${deps_root}"
rsync -a "${project_root}/external/deps_code/webview/" "${deps_root}/"

echo "Adapter applied. Configure src/ohos_sdk locally before gn gen."

