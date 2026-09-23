#!/usr/bin/env bash
# Copyright (c) 2026
# Licensed under the Apache License, Version 2.0.
#
# Stage a Chromium build's runtime into the chromium-ui HarmonyOS project.
#
# The runtime lives in the engine HAR (chromium-ui/engine), not in the entry
# module: a shell depends on that package and gets the engine, its runtime and
# the bridges Chromium calls back into, without owning any of them.
#
# This mirrors stage_native_runtime() in overlay/arkweb/build/build.sh, which is
# the authority on what the shell expects at runtime. It exists because copying
# the obvious files by hand is not enough: the shell reads
# rawfile/chromium/runtime.version before anything else, and a missing or
# malformed manifest surfaces only as "Invalid relative path" from
# resourceManager -- which on a device looks like the browser simply not
# responding.
#
# runtime.version is an aggregate sha256 over everything else in the rawfile
# directory. It is what the shell uses to decide whether its extracted copy of
# the runtime is stale, so it must be regenerated whenever any asset changes.
#
# Usage:
#   scripts/stage-runtime-assets.sh OUT_DIR [PROJECT_DIR]

set -euo pipefail

readonly out_dir="${1:?usage: $0 OUT_DIR [PROJECT_DIR]}"
readonly project="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../overlay/chromium-ui" && pwd)}"
readonly libs_dir="${project}/engine/libs/arm64-v8a"
readonly raw_dir="${project}/engine/src/main/resources/rawfile/chromium"

readonly REQUIRED_PAKS=(
  chrome_100_percent.pak
  chrome_200_percent.pak
  headless_command_resources.pak
  resources.pak
)

die() { printf '%s\n' "$*" >&2; exit 1; }

require_file() { [[ -f "$1" ]] || die "$2 ($1)"; }

clear_directory() {
  mkdir -p "$1"
  find "$1" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
}

require_file "${out_dir}/libweb_engine.so" 'missing libweb_engine.so'
require_file "${out_dir}/libnweb_render.so" 'missing libnweb_render.so'
require_file "${out_dir}/icudtl.dat" 'missing icudtl.dat'
require_file "${out_dir}/snapshot_blob.bin" 'missing V8 startup snapshot'
[[ -d "${out_dir}/locales" ]] || die "missing locales directory (${out_dir}/locales)"
for pak in "${REQUIRED_PAKS[@]}"; do
  require_file "${out_dir}/${pak}" "missing Chromium resource ${pak}"
done

clear_directory "$libs_dir"
clear_directory "$raw_dir"

cp -a "${out_dir}/libweb_engine.so" "${out_dir}/libnweb_render.so" "$libs_dir/"

# ANGLE is a shared library in some configurations and linked statically into
# libweb_engine.so in others. Copy the .so files when the build produced them;
# when it did not, verify the engine really does carry ANGLE internally rather
# than silently shipping a runtime with no GL backend at all.
if [[ -f "${out_dir}/libEGL_angle.so" && -f "${out_dir}/libGLESv2_angle.so" ]]; then
  cp -a "${out_dir}/libEGL_angle.so" "${out_dir}/libGLESv2_angle.so" "$libs_dir/"
  printf 'ANGLE: shared (libEGL_angle.so, libGLESv2_angle.so)\n'
else
  readelf=$(command -v llvm-readelf || command -v readelf) \
    || die 'need readelf to verify static ANGLE linkage'
  if "$readelf" -d "${out_dir}/libweb_engine.so" 2>/dev/null | grep -q 'angle'; then
    die 'libweb_engine.so wants ANGLE shared libraries that the build did not produce'
  fi
  printf 'ANGLE: statically linked into libweb_engine.so\n'
fi

cp -a "${out_dir}/icudtl.dat" "${out_dir}/snapshot_blob.bin" "$raw_dir/"
for pak in "${REQUIRED_PAKS[@]}"; do
  cp -a "${out_dir}/${pak}" "$raw_dir/"
done

# Gendered variants are filtered out: the shell's locale resolver rejects any
# name containing '_', so shipping them only inflates the package.
mkdir -p "${raw_dir}/locales"
find "${out_dir}/locales" -maxdepth 1 -type f -name '*.pak' \
  ! -name '*_FEMININE.pak' ! -name '*_MASCULINE.pak' ! -name '*_NEUTER.pak' \
  -exec cp -a {} "${raw_dir}/locales/" \;

for optional in v8_context_snapshot.bin; do
  [[ -f "${out_dir}/${optional}" ]] && cp -a "${out_dir}/${optional}" "$raw_dir/"
done

# Written last, and after the hash is computed, so it never covers itself.
runtime_version=$(
  cd "$raw_dir"
  find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum |
    sha256sum | awk '{print $1}'
)
[[ "$runtime_version" =~ ^[0-9a-f]{64}$ ]] \
  || die "computed runtime version is not a sha256: ${runtime_version}"
printf '%s\n' "$runtime_version" >"${raw_dir}/runtime.version"

printf 'staged %s locale paks\n' "$(find "${raw_dir}/locales" -type f | wc -l)"
printf 'runtime.version %s\n' "$runtime_version"
printf 'staged runtime from %s\n' "$out_dir"
