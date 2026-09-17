# Chromium for HarmonyOS Adapter

[中文说明](README_ZH.md)

This branch publishes the HarmonyOS adaptation used to build the local
Chromium 150 test HAP. It is an independent, unofficial project and is not
endorsed by Google, Chromium, Huawei, or the OpenHarmony project.

## Baseline

- Chromium: `150.0.7871.114`
- Upstream revision: `f405107495a07cb1bfcf687d4af8d91117098db6`
- Target OS: `ohos` (presented to Chromium as HarmonyOS)
- Target CPU: `arm64`
- Product: `plan_kirin_pc`
- V8: JITless, WebAssembly interpreter enabled
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

- `patches/chromium-150-harmonyos.patch`: all modifications to files tracked by
  upstream Chromium.
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
- `scripts/apply-adapter.sh`: applies this snapshot to the pinned Chromium
  revision.
- `.github/workflows/upstream-stable.yml`: checks official Chromium Stable
  every six hours and can dispatch a licensed self-hosted HAP build.
- `licenses/`: applicable license texts and the generated third-party credits.
- `SOURCE_MANIFEST.sha256`: SHA-256 checksums for every published regular file
  except the manifest itself.

No HAP, native build output, HarmonyOS SDK, signing certificate, provisioning
profile, device UDID, private key, or local signing configuration is published.
Developers must configure their own HarmonyOS SDK and signing identity.

## Apply

Prepare a complete Chromium checkout at the pinned revision, including its
DEPS, then run:

```bash
./scripts/apply-adapter.sh /path/to/chromium/src
cp config/args.plan_kirin_pc.gn /path/to/chromium/src/out/plan_kirin_pc/args.gn
```

Read [BUILDING.md](BUILDING.md) before generating or compiling the target.
DRM and codec boundaries are documented in [docs/DRM_AND_MEDIA.md](docs/DRM_AND_MEDIA.md),
and the account/sync trust model is in [docs/HARMONYOS_SYNC.md](docs/HARMONYOS_SYNC.md).

## Licensing

Chromium is primarily BSD-3-Clause and includes third-party code under other
licenses. This build includes LGPL and MPL components. See
[OPEN_SOURCE_NOTICES.md](OPEN_SOURCE_NOTICES.md), [SOURCE_OFFER.md](SOURCE_OFFER.md),
and `licenses/THIRD_PARTY_LICENSES.html`.
