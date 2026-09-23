#!/usr/bin/env bash
# Copyright (c) 2026
# Licensed under the Apache License, Version 2.0.

set -euo pipefail
shopt -s nullglob

SCRIPT_PATH=$(readlink -f "${BASH_SOURCE[0]}")
SCRIPT_DIR=$(cd "$(dirname "$SCRIPT_PATH")" && pwd)
ROOT_DIR=$(cd "${SCRIPT_DIR}/../.." && pwd)
OUT_DIR="${ROOT_DIR}/out/plan_kirin_pc"
HAP_PROJECT="${ROOT_DIR}/chromium-ui"
GN_BIN="${CHROMIUM_GN:-${ROOT_DIR}/buildtools/linux64/gn}"
NINJA_BIN="${CHROMIUM_NINJA:-${ROOT_DIR}/third_party/ninja/ninja}"

export TMPDIR="${TMPDIR:-${HOME}/.cache/chromium-link-tmp}"
mkdir -p "$TMPDIR"

JOBS=8
PACKAGE_HAP=0
SKIP_NINJA="${CHROMIUM_SKIP_NINJA:-0}"
EXTRA_GN_ARGS=""
HVIGOR_PROJECT_RESULT=""
HAP_PRODUCT="${CHROMIUM_HAP_PRODUCT:-default}"
HAP_BUILD_MODE="${CHROMIUM_HAP_BUILD_MODE:-release}"
REQUIRED_RUNTIME_PAKS=(
  chrome_100_percent.pak
  chrome_200_percent.pak
  headless_command_resources.pak
  resources.pak
)

usage() {
  cat <<'EOF'
Usage: ./ohos_build/build.sh [options] [device]

Every device maps to the single plan_kirin_pc / kirin_pc arm64 target.

Options:
  -j N        Number of Ninja jobs (default: 8)
  -t b|n|w   Build and stage the native Chromium runtime
  -t p       Build, stage, sign, and validate Chromium.Hap with devecocli
  -G ARGS     Append GN arguments
  -A          Accepted for compatibility; still builds plan_kirin_pc
  -h          Show this help

Accepted device aliases:
  plan_kirin_pc kirin_pc phone tablet 2in1 tv car foldable
EOF
}

die() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

while (($#)); do
  case "$1" in
    -j)
      (($# >= 2)) || die "-j requires a value"
      JOBS="$2"
      shift 2
      ;;
    -t)
      (($# >= 2)) || die "-t requires a value"
      case "$2" in
        p)
          PACKAGE_HAP=1
          ;;
        b|n|w|libweb_engine|libnweb_render)
          ;;
        *)
          die "unsupported target '$2'; use b, n, w, or p"
          ;;
      esac
      shift 2
      ;;
    -G)
      (($# >= 2)) || die "-G requires a value"
      EXTRA_GN_ARGS+=" $2"
      shift 2
      ;;
    -A|-artifact)
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    plan_kirin_pc|kirin_pc|phone|tablet|2in1|tv|car|foldable|desktop|rk3568|rk3568_64|musl_64|x86_64)
      shift
      ;;
    *)
      die "unknown argument '$1'"
      ;;
  esac
done

[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "invalid job count '$JOBS'"
[[ -x "$GN_BIN" ]] || die "GN not found: $GN_BIN"
[[ -x "$NINJA_BIN" ]] || die "Ninja not found: $NINJA_BIN"

GN_ARGS=$(cat <<'EOF'
target_os="ohos"
target_cpu="arm64"
is_debug=false
is_official_build=true
is_component_build=false
proprietary_codecs=true
ffmpeg_branding="Chrome"
optimize_for_size=true
symbol_level=0
blink_symbol_level=0
v8_symbol_level=0
use_icf=false
use_musl=true
use_ohos_sdk_sysroot=true
product_name="kirin_pc"
enable_arkweb=false
enable_library_cdms=false
enable_lens_desktop=false
enable_lens_desktop_google_branded_features=false
enable_glic_internal_resources=false
safe_browsing_mode=0
safe_browsing_use_unrar=false
enable_pdf=true
enable_pdf_ink2=false
enable_pdf_save_to_drive=false
enable_basic_print_dialog=false
enable_screen_ai_service=false
enable_widevine=false
enterprise_cloud_content_analysis=false
enterprise_client_certificates=true
enterprise_local_content_analysis=false
use_aura=true
toolkit_views=true
use_ozone=true
ozone_auto_platforms=false
ozone_platform="ohos"
ozone_platform_headless=false
use_alsa=false
use_pulseaudio=false
use_v8_context_snapshot=false
v8_enable_lite_mode=true
v8_jitless=true
v8_enable_sparkplug=false
v8_enable_maglev=false
v8_enable_turbofan=false
v8_enable_webassembly=true
v8_enable_drumbrake=true
v8_drumbrake_bounds_checks=true
v8_control_flow_integrity=true
v8_enable_builtins_optimization=false
pdf_enable_v8=false
build_with_tflite_lib=true
use_fake_screen_ai=false
dawn_use_swiftshader=false
enable_swiftshader=false
angle_libs_suffix="_angle"
angle_enable_vulkan=true
angle_enable_swiftshader=false
angle_use_custom_libvulkan=false
root_extra_deps=[
  "//cef:libweb_engine",
  "//chrome:packed_resources",
  "//ohos_nweb/src/aura_shell:libnweb_render",
]
EOF
)

runtime_out_dir() {
  local override="${CHROMIUM_RUNTIME_OUT_DIR:-}"
  if [[ -z "$override" ]]; then
    printf '%s\n' "$OUT_DIR"
  elif [[ "$override" = /* ]]; then
    printf '%s\n' "${override%/}"
  else
    printf '%s\n' "${ROOT_DIR}/${override%/}"
  fi
}

require_file() {
  [[ -f "$1" ]] || die "$2: $1"
}

require_dir() {
  [[ -d "$1" ]] || die "$2: $1"
}

require_min_size() {
  local size
  size=$(stat -c '%s' "$1")
  ((size >= $2)) || die "$1 is ${size} bytes; expected at least $2"
}

require_aarch64() {
  readelf -h "$1" | grep 'Machine:.*AArch64' >/dev/null ||
    die "$1 is not an AArch64 ELF artifact"
}

reject_stub_symbols() {
  if strings "$1" | grep -Eq 'web_engine_napi_stub|web_engine_stub|debug_stub'; then
    die "$1 contains a legacy stub marker"
  fi
}

generate_revision_headers() {
  python3 "${ROOT_DIR}/build/util/lastchange.py" \
    -m SKIA_COMMIT_HASH \
    -s "${ROOT_DIR}/third_party/skia" \
    --header "${ROOT_DIR}/skia/ext/skia_commit_hash.h"
  python3 "${ROOT_DIR}/build/util/lastchange.py" \
    -m DAWN_COMMIT_HASH \
    -s "${ROOT_DIR}/third_party/dawn" \
    --revision "${ROOT_DIR}/gpu/webgpu/DAWN_VERSION" \
    --header "${ROOT_DIR}/gpu/webgpu/dawn_commit_hash.h"
}

validate_native_runtime() {
  local out_dir="$1"
  local pak

  require_file "${out_dir}/libweb_engine.so" "missing real libweb_engine.so"
  require_file "${out_dir}/libnweb_render.so" "missing real libnweb_render.so"
  require_file "${out_dir}/snapshot_blob.bin" "missing V8 startup snapshot"
  require_file "${out_dir}/libEGL_angle.so" "missing Chromium ANGLE EGL runtime"
  require_file "${out_dir}/libGLESv2_angle.so" "missing Chromium ANGLE GLES runtime"
  require_file "${out_dir}/icudtl.dat" "missing icudtl.dat"
  require_dir "${out_dir}/locales" "missing Chromium locales"

  for pak in "${REQUIRED_RUNTIME_PAKS[@]}"; do
    require_file "${out_dir}/${pak}" "missing Chromium resource ${pak}"
  done

  require_min_size "${out_dir}/libweb_engine.so" 5242880
  require_min_size "${out_dir}/libnweb_render.so" 16384
  require_min_size "${out_dir}/libEGL_angle.so" 131072
  require_min_size "${out_dir}/libGLESv2_angle.so" 1048576

  require_aarch64 "${out_dir}/libweb_engine.so"
  require_aarch64 "${out_dir}/libnweb_render.so"
  require_aarch64 "${out_dir}/libEGL_angle.so"
  require_aarch64 "${out_dir}/libGLESv2_angle.so"
  reject_stub_symbols "${out_dir}/libweb_engine.so"
  reject_stub_symbols "${out_dir}/libnweb_render.so"

  nm -D "${out_dir}/libweb_engine.so" |
    grep 'RegisterWebEngineModule' >/dev/null ||
    die "libweb_engine.so does not export RegisterWebEngineModule"
  nm -D "${out_dir}/libweb_engine.so" |
    grep 'ChromiumHarmonyOSNativeChildMain' >/dev/null ||
    die "libweb_engine.so does not export the Chromium child runtime"
  nm -D "${out_dir}/libnweb_render.so" |
    grep 'ChromiumNativeChildMain' >/dev/null ||
    die "libnweb_render.so does not export ChromiumNativeChildMain"
  nm -D "${out_dir}/libnweb_render.so" | grep 'NWebRenderMain' >/dev/null ||
    die "libnweb_render.so does not export NWebRenderMain"
  strings "${out_dir}/libnweb_render.so" | grep 'libweb_engine.so' >/dev/null ||
    die "libnweb_render.so is not linked to the Chromium runtime bootstrap"
}

clear_directory() {
  mkdir -p "$1"
  find "$1" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
}

stage_native_runtime() {
  local out_dir="$1"
  local libs_dir="${HAP_PROJECT}/engine/libs/arm64-v8a"
  local raw_dir="${HAP_PROJECT}/engine/src/main/resources/rawfile/chromium"
  local runtime_version

  validate_native_runtime "$out_dir"
  rm -rf "${HAP_PROJECT}/entry/.cxx" "${HAP_PROJECT}/entry/build"
  clear_directory "$libs_dir"
  clear_directory "$raw_dir"

  cp -a "${out_dir}/libweb_engine.so" "${out_dir}/libnweb_render.so" "$libs_dir/"
  cp -a "${out_dir}/libEGL_angle.so" "${out_dir}/libGLESv2_angle.so" "$libs_dir/"
  cp -a "${out_dir}/icudtl.dat" "${out_dir}/snapshot_blob.bin" "$raw_dir/"
  for pak in "${REQUIRED_RUNTIME_PAKS[@]}"; do
    cp -a "${out_dir}/${pak}" "$raw_dir/"
  done
  mkdir -p "${raw_dir}/locales"
  find "${out_dir}/locales" -maxdepth 1 -type f -name '*.pak' \
    ! -name '*_FEMININE.pak' ! -name '*_MASCULINE.pak' \
    ! -name '*_NEUTER.pak' -exec cp -a {} "${raw_dir}/locales/" \;

  for optional_file in v8_context_snapshot.bin; do
    if [[ -f "${out_dir}/${optional_file}" ]]; then
      cp -a "${out_dir}/${optional_file}" "$raw_dir/"
    fi
  done

  runtime_version=$(
    cd "$raw_dir"
    find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum |
      sha256sum | awk '{print $1}'
  )
  printf '%s\n' "$runtime_version" >"${raw_dir}/runtime.version"

  printf 'Staged verified Chromium runtime from %s\n' "$out_dir"
}

find_signed_hap() {
  local project_dir="$1"
  local hap
  hap=$(find "${project_dir}/entry/build" -type f -name '*-signed.hap' | sort | tail -n 1)
  if [[ -z "$hap" ]]; then
    hap=$(find "${project_dir}/entry/build" -type f -name '*.hap' | sort | tail -n 1)
  fi
  [[ -n "$hap" ]] || die "Hvigor did not produce a HAP"
  printf '%s\n' "$hap"
}

run_devecocli() {
  local project_dir="$HAP_PROJECT"

  if command -v devecocli >/dev/null 2>&1 &&
    command -v node >/dev/null 2>&1; then
    (cd "$HAP_PROJECT" && devecocli build --product "$HAP_PRODUCT" \
      --build-mode "$HAP_BUILD_MODE")
    HVIGOR_PROJECT_RESULT="$project_dir"
    return
  fi

  command -v wslpath >/dev/null 2>&1 ||
    die "devecocli is unavailable and WSL path conversion is unavailable"
  command -v rsync >/dev/null 2>&1 || die "rsync is required for HAP staging"
  command -v powershell.exe >/dev/null 2>&1 ||
    die "devecocli is unavailable from WSL and Windows PowerShell was not found"

  local stage_dir="${CHROMIUM_HAP_STAGING_DIR:-}"
  local stage_dir_win

  if [[ -z "$stage_dir" ]]; then
    if [[ -d /mnt/e ]]; then
      stage_dir="/mnt/e/chromium-hmos-hap-stage/chromium-ui"
    else
      stage_dir="/mnt/c/chromium-hmos-hap-stage/chromium-ui"
    fi
  fi

  mkdir -p "$stage_dir"
  stage_dir=$(readlink -f "$stage_dir")
  [[ "$stage_dir" != "/" && "$stage_dir" != "$HAP_PROJECT" ]] ||
    die "refusing to use unsafe HAP staging directory: $stage_dir"
  rm -rf "${stage_dir}/.hvigor" "${stage_dir}/entry/.cxx" \
    "${stage_dir}/entry/build"
  rsync -a --delete --exclude '.hvigor/' --exclude 'entry/.cxx/' \
    --exclude 'entry/build/' "${HAP_PROJECT}/" "${stage_dir}/"
  stage_dir_win=$(wslpath -w "$stage_dir")
  powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \
    "Set-Location -LiteralPath '${stage_dir_win}'; & devecocli.ps1 build --product '${HAP_PRODUCT}' --build-mode '${HAP_BUILD_MODE}'; exit \$LASTEXITCODE"
  HVIGOR_PROJECT_RESULT="$stage_dir"
}

require_hap_entry() {
  local entries="$1"
  local pattern="$2"
  grep -Eq "$pattern" <<<"$entries" || die "HAP is missing entry matching: $pattern"
}

validate_hap() {
  local hap="$1"
  local entries
  local pak
  local unpack_dir
  local runtime_version
  local unexpected_root_paks

  entries=$(unzip -Z1 "$hap")
  require_hap_entry "$entries" '^libs/arm64-v8a/libweb_engine\.so$'
  require_hap_entry "$entries" '^libs/arm64-v8a/libnweb_render\.so$'
  require_hap_entry "$entries" '^libs/arm64-v8a/libEGL_angle\.so$'
  require_hap_entry "$entries" '^libs/arm64-v8a/libGLESv2_angle\.so$'
  require_hap_entry "$entries" '^resources/rawfile/chromium/icudtl\.dat$'
  require_hap_entry "$entries" '^resources/rawfile/chromium/snapshot_blob\.bin$'
  require_hap_entry "$entries" '^resources/rawfile/chromium/runtime\.version$'
  for pak in "${REQUIRED_RUNTIME_PAKS[@]}"; do
    require_hap_entry "$entries" "^resources/rawfile/chromium/${pak//./\\.}$"
  done
  require_hap_entry "$entries" '^resources/rawfile/chromium/locales/[^/]+\.pak$'

  unexpected_root_paks=$(
    grep -E '^resources/rawfile/chromium/[^/]+\.pak$' <<<"$entries" |
      grep -Ev '^resources/rawfile/chromium/(chrome_100_percent|chrome_200_percent|headless_command_resources|resources)\.pak$' ||
      true
  )
  [[ -z "$unexpected_root_paks" ]] ||
    die "HAP contains unexpected root Chromium .pak files: ${unexpected_root_paks//$'\n'/, }"

  if grep -Eq '^resources/rawfile/chromium/README' <<<"$entries"; then
    die "HAP still contains placeholder Chromium README content"
  fi
  if grep -Eq '^resources/rawfile/chromium/(chrome|web_render)$' <<<"$entries"; then
    die "HAP still contains unused standalone Chromium executables"
  fi
  if grep -Eq '^resources/rawfile/chromium/locales/.*\.pak\.info$' <<<"$entries"; then
    die "HAP still contains locale build metadata"
  fi

  runtime_version=$(unzip -p "$hap" 'resources/rawfile/chromium/runtime.version' |
    tr -d '\r\n')
  [[ "$runtime_version" =~ ^[0-9a-f]{64}$ ]] ||
    die "HAP contains an invalid Chromium runtime version manifest"

  unpack_dir=$(mktemp -d)
  unzip -q "$hap" 'libs/arm64-v8a/libweb_engine.so' \
    'libs/arm64-v8a/libnweb_render.so' \
    'libs/arm64-v8a/libEGL_angle.so' \
    'libs/arm64-v8a/libGLESv2_angle.so' -d "$unpack_dir"
  require_min_size "${unpack_dir}/libs/arm64-v8a/libweb_engine.so" 5242880
  require_min_size "${unpack_dir}/libs/arm64-v8a/libnweb_render.so" 16384
  require_min_size "${unpack_dir}/libs/arm64-v8a/libEGL_angle.so" 131072
  require_min_size "${unpack_dir}/libs/arm64-v8a/libGLESv2_angle.so" 1048576
  require_aarch64 "${unpack_dir}/libs/arm64-v8a/libweb_engine.so"
  require_aarch64 "${unpack_dir}/libs/arm64-v8a/libnweb_render.so"
  require_aarch64 "${unpack_dir}/libs/arm64-v8a/libEGL_angle.so"
  require_aarch64 "${unpack_dir}/libs/arm64-v8a/libGLESv2_angle.so"
  reject_stub_symbols "${unpack_dir}/libs/arm64-v8a/libweb_engine.so"
  reject_stub_symbols "${unpack_dir}/libs/arm64-v8a/libnweb_render.so"
  nm -D "${unpack_dir}/libs/arm64-v8a/libweb_engine.so" |
    grep 'ChromiumHarmonyOSNativeChildMain' >/dev/null ||
    die "HAP libweb_engine.so is missing the Chromium child runtime"
  nm -D "${unpack_dir}/libs/arm64-v8a/libnweb_render.so" |
    grep 'ChromiumNativeChildMain' >/dev/null ||
    die "HAP libnweb_render.so is missing the native child entry"
  nm -D "${unpack_dir}/libs/arm64-v8a/libnweb_render.so" |
    grep 'NWebRenderMain' >/dev/null ||
    die "HAP libnweb_render.so is missing the legacy child entry"
  rm -rf "$unpack_dir"
}

package_hap() {
  local hvigor_project
  local built_hap
  local output_hap="${OUT_DIR}/Chromium.Hap"

  rm -f "$output_hap"
  run_devecocli
  hvigor_project="$HVIGOR_PROJECT_RESULT"
  built_hap=$(find_signed_hap "$hvigor_project")
  cp -a "$built_hap" "$output_hap"
  validate_hap "$output_hap"
  printf 'Chromium.Hap: %s\n' "$output_hap"
  sha256sum "$output_hap"
}

mkdir -p "$OUT_DIR"
generate_revision_headers
printf 'Generating fixed plan_kirin_pc GN graph...\n'
"$GN_BIN" gen "$OUT_DIR" --args="${GN_ARGS}${EXTRA_GN_ARGS}"

if [[ "$SKIP_NINJA" != 1 ]]; then
  printf 'Building Chromium native runtime with %s jobs...\n' "$JOBS"
  "$NINJA_BIN" -C "$OUT_DIR" -j "$JOBS" \
    libweb_engine libnweb_render chrome:packed_resources libEGL libGLESv2
fi

RUNTIME_OUT=$(runtime_out_dir)
stage_native_runtime "$RUNTIME_OUT"

if ((PACKAGE_HAP)); then
  package_hap
fi
