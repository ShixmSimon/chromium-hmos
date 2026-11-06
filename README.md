# Chromium for HarmonyOS Adapter

[中文说明](README_ZH.md)

A Chromium adaptation for HarmonyOS that provides the browser engine used by
[Lumie](https://github.com/miramira8295/Lumie). This repository contains native
platform patches, integration sources, the engine bridge and build tooling;
Lumie's application UI is maintained in its own repository.

The active branch is **`hmos-155-adapter`**, based on Chromium **155**.
`hmos-154-adapter` retains the earlier 154 adaptation. This is an independent,
unofficial project, not endorsed by Google, Chromium, Huawei or OpenHarmony.

[Build guide](BUILDING.md) · [Engine downloads](https://github.com/miramira8295/chromium-hmos/releases) · [Native build CI](https://github.com/miramira8295/chromium-hmos/actions/workflows/incremental-build.yml) · [Third-party notices](OPEN_SOURCE_NOTICES.md)

## Capabilities and status

| Area | Integration |
| --- | --- |
| Devices and UI | Phone, tablet and 2in1 routes; ArkUI shell bridge and Chromium Aura/Views integration. |
| JavaScript and WebAssembly | Runtime executable-memory probing selects JIT or JITless; DrumBrake provides WebAssembly in JITless mode. |
| Graphics and media | OpenGL/Vulkan adaptations, HarmonyOS media decoding and HDR integration. Format and HDR availability depend on the device, system and selected rendering path. |
| Developer tools | Chromium DevTools and local CDP access; Lumie provides the mobile menu and expandable floating entry. |
| Content protection | Native ad filtering, local phishing-rule integration and signed Chromium data-component updates. |
| Distribution | CI publishes an unsigned `engine.har`, native symbols and checksums; downstream apps package and sign the engine. |

### Chromium data components

`chrome://components/` exposes an explicit allowlist: CRLSet, PKI Metadata,
Safety Tips, Certificate Error Assistant, File Type Policies, Origin Trials,
Hyphenation and Crowd Deny. Updates retain HTTPS and Chromium's pinned CRX
publisher verification. Origin Trials changes are read at browser restart;
Crowd Deny supplies a local notification-permission warning in Lumie.

Registration does not establish download or activation. The 2026-11-05 official
OHOS update check offered Hyphenation and Crowd Deny, while File Type Policies
and Origin Trials returned no update and retained builtin/default behavior.
The integrated 155 native build and Lumie packaging passed; device-level
activation checks are tracked in the [component notes](docs/OHOS_SECURITY_COMPONENT_UPDATES.md).

Independent video-layer promotion remains experimental; Lumie currently keeps
it disabled and uses Chromium's webpage compositor for video. See the
[video-layer notes](docs/OHOS_VIDEO_OVERLAY.md). Widevine CDM is not included;
codec and DRM boundaries are described in [media policy](docs/DRM_AND_MEDIA.md).

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
- `scripts/prepare-chromium-155.sh`: prepares the isolated self-hosted 155 checkout.
- `.github/workflows/`: GitHub Actions adapter checks, incremental native builds,
  runtime inspection and stable tracking. Native builds use the existing
  `self-hosted, harmonyos-chromium` runner.
- `_bak/depot-workflows/`: archived Depot workflows (not automatically executed);
  see [docs/DEPOT_CI.md](docs/DEPOT_CI.md) for historical setup.
- `licenses/`: applicable license texts and the generated third-party credits.
- `SOURCE_MANIFEST.sha256`: the original source-publication checksum list;
  current migration inputs are pinned by Git and the migration manifest.

The source tree excludes the private HarmonyOS SDK and signing material.
CI packages the unsigned engine HAR and publishes it with native symbols and
checksums as a GitHub prerelease. HAP signing stays on the developer machine.

## Build through CI

Push source changes to `hmos-155-adapter`. GitHub Actions prepares the isolated
`/root/chromium-155/src` tree, builds the native runtime and packages the engine
HAR. Both release branches share the existing self-hosted runner and serialize
native work through the same concurrency group. Manual dispatch can upload a
verification HAR without publishing a prerelease.

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
