# TreesInViz experiment (2026-10-19)

Status: opt-in shell control built; device validation pending. Do not treat the
previous Graphite video acceptance as TreesInViz acceptance.

## Reproducible baseline

- Chromium revision: `b510e9d7cd3a2fbd78d0ddc42234103206c5f78d`.
- Native adapter: `680d1a992e9070810a31f83884c632a256258189`, including the
  phone Graphite fullscreen resource-return fix, based on `827c3ef4`.
- Native CI: https://github.com/miramira8295/chromium-hmos/actions/runs/39197840196.
- Lumie experiment: `00fef900e312353d02586a0293ca6992a68d5e4f`, branch
  `codex/trees-in-viz`, based on `ab3b152`.

The first comparison uses identical native libraries with TreesInViz off/on.
No native code changes are required just to request the existing upstream
feature. Whether more OHOS adaptation is needed remains a device-test question.

## What the switch does

`lumie://flags` exposes TreesInViz, off by default and requiring a full restart.
The shell sends `TreesInViz` alongside `OhosZeroCopyVideo` in one enable-features
list only when the startup configuration selects Graphite with a separate GPU.
Safe start and non-Graphite selections suppress the stored opt-in. The separate
TreeAnimationsInViz experiment is not enabled. Actual GPU fallback and actual
TreesInViz activation must be checked, not inferred from the saved preference.

## Source audit

At the pinned upstream revision:

- `cc/base/features.cc` defines TreesInViz disabled by default.
- `cc/trees/layer_tree_host_impl.cc` rebuilds frame data in Viz when enabled.
- `cc/mojo_embedder/viz_layer_context.cc` serializes texture resources through
  TransferableResource and sends DisplayColorSpaces.
- `components/viz/service/layers/layer_context_impl.cc` accepts SurfaceLayer
  and TextureLayer, but rejects a direct VideoLayer. Ordinary media playback in
  `third_party/blink/renderer/platform/media/web_media_player_impl.cc` activates
  SurfaceLayer on video metadata. Other media entry points still need coverage.
- `patches/ohos-phone-vulkan-video.patch` already includes OHOS Vulkan YCbCr
  metadata in TransferableResource and its Mojo serialization.
- Graphite output's P3/HDR10 probing and native-buffer tagging remain in
  `overlay/components/viz/service/display_embedder/skia_output_device_ohos_graphite.cc`.

The primary integration risks are resource lifetime/return across the new tree
submission path, video surface visibility/activation, and color usage/metadata
through intermediate render passes. Existing Dawn completion fences and decoder
leases must remain authoritative; do not infer safety just from a displayed frame.

## Validation sequence

1. Install the signed phone HAP, confirm the default switch is off and capture
   GraphiteDawnVulkan/zero GPU crashes. Capture an off-mode scrolling baseline.
2. Enable TreesInViz and restart completely. Require both chrome://gpu showing
   TreesInViz enabled and trace activity from VizLayerContext/LayerContextImpl.
3. Verify ordinary pages, native scrolling, tab switching, resize/fullscreen and
   30-second background recovery. Capture identical scrolling workloads in off/on
   modes with repeated runs; compare frame pacing and renderer/Viz CPU, not just
   a JavaScript benchmark score.
4. Repeat the HEVC Main10 SDR (3 min), PQ (10 min), fullscreen (5 rounds), seek /
   SDR-HDR switching (5 rounds), background and minimum-brightness acceptance.
   Require CPU output copies=0 and all five existing native video trace markers.
5. Add explicit CSS Display-P3 and Canvas Display-P3 samples, including mixed
   sRGB/P3 content and P3-SDR/PQ transitions. Verify actual P3 output selection and
   restoration; screenshots alone do not measure panel gamut accuracy.
6. Verify opt-out/restart and safe-start recovery. Keep the experiment off by
   default until compatibility and repeatable performance benefits are established.

## Checks completed

- 24 Node tests passed: experiment gating/zero-copy preservation and existing
  process-mode restart/recovery policy.
- Focused ArkTS check: four changed source files, no errors. The initial check
  before worktree dependencies were installed reported missing imports; it was
  superseded by the check after dependency installation and the successful builds.
- Phone and tablet signed HAP builds passed. Both packages' libweb_engine.so and
  libnweb_render.so match the native CI HAR byte for byte.
- Native libraries were reused rather than rebuilt, so this is not a new native CI run.
- HDC reported no connected devices; install, smoke test, P3/video regressions,
  actual TreesInViz activation and performance comparison are still pending.

Local evidence: `chromium-hmos/artifacts/trees-in-viz-review/`, including
`package-proof.json`, `arkts-focused.log`, `signed-build.log` and pinned upstream
source snapshots. The previous video baseline is in
`chromium-hmos/artifacts/pq-fullscreen-fix/REPORT.md`.
