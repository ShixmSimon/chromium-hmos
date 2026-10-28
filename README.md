# Chromium for HarmonyOS Adapter

[中文说明](README_ZH.md)

This branch targets the Chromium 155 HarmonyOS migration. The stable 154
build stays on `hmos-154-adapter`. It is an independent, unofficial project and is not
endorsed by Google, Chromium, Huawei, or the OpenHarmony project.

## Baseline

- Chromium: `155.0.8059.91`
- Upstream revision: `f043649a0ef1fd96e48d56ddbfc8a4a99aa2ca37`
- Target OS: `ohos` (presented to Chromium as HarmonyOS)
- Target CPU: `arm64`
- Product: `kirin_pc`; CI output: `out/ohos_arm64`
- V8: JIT compilers in the build, used when the launch can map executable memory (tablets, 2in1); JITless otherwise (released phone packages), where WebAssembly runs on the DrumBrake interpreter
- UI: Aura/Views on large screens; ArkUI mobile shell on phones
- Media: AAC/H.264 build flags enabled; Widevine CDM disabled
- Public identity: Chromium HarmonyOS Adapter (`com.caidingding233.chromium`).
  Product brands and AppGallery signing remain in separate downstream trees.

## HarmonyOS Integration

- Phone and folded-phone windows use a mobile user agent and an ArkUI browser
  bar; tablets, PCs, TVs, cars, and expanded foldables retain desktop content
  behavior with the configured large-screen Chromium UI family.
- PWA launches use a separate, non-exported `PWASmallAppExperience` UIAbility
  with an immersive capsule and the native Chromium PWA menu model.
- The ArkUI bridge exposes HarmonyOS share, cast, print, file selection, default
  browser selection, AppGallery update, Huawei account, and Wallet entry points.
- The Ozone input path separates mouse and touch state, resets stale capture on
  lifecycle changes, and forwards IME focus through the HarmonyOS input method.
- CDP and the local browser-content MCP bridge are loopback-only by default.

## Repository Layout

- `patches/migration-155/`: the complete native and DEPS adaptations rebased
  onto the pinned 155 revisions; `manifest.json` records provenance and hashes.
- `patches/chromium-154-harmonyos.patch`: the historical 154 baseline.
- `patches/deps/`: OHOS changes to DEPS repositories (V8, ANGLE, Dawn, FFmpeg,
  PDFium, Skia, WebRTC, and others), listed in `patches/deps/series`. See
  [docs/OHOS_DEVICE_FIXES.md](docs/OHOS_DEVICE_FIXES.md).
- `tools/macos-container/`: build and deploy scripts for Apple silicon macOS
  through Apple `container` and Rosetta; see
  [docs/BUILDING_MACOS_CONTAINER.md](docs/BUILDING_MACOS_CONTAINER.md).
- `overlay/`: new HarmonyOS, ArkWeb, Ozone, Aura shell, and ArkUI source files.
- `external/deps_code/webview/`: WebView interface sources required by the
  adapter.
- `config/args.plan_kirin_pc.gn`: GN arguments used by the tested native build.
- `scripts/prepare-depot-155.sh`: prepares the isolated 155 checkout on Depot.
- `.depot/workflows/`: adapter checks and cached native builds for 154/155;
  see [docs/DEPOT_CI.md](docs/DEPOT_CI.md).
- `_bak/workflows/`: archived GitHub Actions workflows (not executed).
- `licenses/`: applicable license texts and the generated third-party credits.
- `SOURCE_MANIFEST.sha256`: the original source-publication checksum list;
  current migration inputs are pinned by Git and the migration manifest.

The source tree excludes the private HarmonyOS SDK and signing material.
Native runtime and symbol archives are uploaded as Depot CI artifacts.

## Build through CI

Push source changes to `hmos-155-adapter`. CI prepares `/mnt/chromium-155/source/src`,
synchronizes the pinned dependencies and toolchain, applies the migration,
then builds and packages the native runtime. Source, SDK and compiled objects
are cached independently of 154. HAR/HAP packaging remains a separate step.

See [BUILDING.md](BUILDING.md) and the
[migration notes](docs/CHROMIUM_155_MIGRATION.md) for the branch and path
contract. `scripts/apply-adapter.sh` is retained for the historical 154 tree.
DRM and codec boundaries are documented in [docs/DRM_AND_MEDIA.md](docs/DRM_AND_MEDIA.md),
and the account/sync trust model is in [docs/HARMONYOS_SYNC.md](docs/HARMONYOS_SYNC.md).

## Licensing

Chromium is primarily BSD-3-Clause and includes third-party code under other
licenses. This build includes LGPL and MPL components. See
[OPEN_SOURCE_NOTICES.md](OPEN_SOURCE_NOTICES.md), [SOURCE_OFFER.md](SOURCE_OFFER.md),
and `licenses/THIRD_PARTY_LICENSES.html`.
