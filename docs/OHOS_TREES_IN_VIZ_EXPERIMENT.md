# TreesInViz experiment (2026-10-19)

Status: phone compatibility checks passed with TreesInViz actually enabled.
Testing ended at the user’s request; the OFF performance comparison was not run.
There is no established performance benefit yet. The flag remains default-off;
the user’s phone retains its explicit ON preference.

## Reproducible baseline

- Chromium revision: `b510e9d7cd3a2fbd78d0ddc42234103206c5f78d`.
- Native adapter: `680d1a992e9070810a31f83884c632a256258189`, including the
  phone Graphite fullscreen resource-return fix, based on `827c3ef4`.
- Native CI: https://github.com/miramira8295/chromium-hmos/actions/runs/39197840196.
- Lumie experiment: originally `00fef900e312353d02586a0293ca6992a68d5e4f`,
  integrated into `main` as `5c0c75d` and merged with its original history.
- Native fix is also integrated into the default `hmos-154-adapter` branch.
  Development and testing now use the primary workspaces; merged topic branches
  are removed at the user's request.

The first comparison uses identical native libraries with TreesInViz off/on.
No native code changes are required just to request the existing upstream
feature. The tested phone paths passed without additional TreesInViz-specific native changes;
this does not establish coverage for other devices or media entry points.

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
- The primary-workspace phone build installed with `devecocli run` (Smoke: PASS).
  Installed native libraries match the verified `680d1a99` HAR. Subsequent default
  branch native CI [39213533801](https://github.com/miramira8295/chromium-hmos/actions/runs/39213533801)
  and adapter CI [39213533918](https://github.com/miramira8295/chromium-hmos/actions/runs/39213533918)
  passed; those results do not mean a newer native artifact was installed.
- Mate 70 Pro+: actual `GraphiteDawnVulkan`, `trees_in_viz=enabled_on`, separate
  GPU process and zero reported GPU crashes. Scroll trace: 774 tree sends,
  774 Viz updates and 773 draws.
- HEVC Main10 SDR: 185.698 seconds, 5558 frames, 11 dropped (0.198%).
  HEVC Main10 PQ: 619.212 seconds, 18539 frames, 38 dropped (0.205%).
  Both used verified 1280×720, 30 fps, 10-bit HEVC clips looped without audio.
- SDR and PQ fullscreen enter/exit: five rounds each passed. Five rounds of
  seeking and SDR/HDR switching, and 30-second video background recovery passed.
- Native logs report `CPU output copies=0`. Video traces include
  `OhosGraphiteDecoderBufferImported`, `OhosNativeVideoReadResume`,
  `OhosNativeVideoReadSuspend`, `OhosNativeVideoLeaseCompleted`, and
  `OhosGraphiteVideoMetadataReceived`, alongside TreesInViz activity.
  Continuous traces use a 64 MiB ring buffer and retain the tail; independent
  playback samples establish the full playback duration.
- CSS/Canvas P3 and sRGB selection passed three rounds. Native output modes
  alternate P3 SDR10 / SDR10, and mixed P3/PQ playback transitions
  P3 SDR10 → HDR10 → P3 SDR10, then ordinary page → SDR10.
- At minimum brightness 1 with auto brightness off, five ordinary-page
  background/resume rounds (30 seconds each) passed. Gray patches matched the
  baseline after each resume and after exiting HDR. Brightness was restored to
  the session’s original 108 with auto brightness off.
- Three ON scrolling runs completed without rAF intervals over 33.34 ms.
  These are renderer callbacks, not physical presentation FPS. OFF runs were
  cancelled at the user’s request, so no A/B performance conclusion is claimed.
- Testing stopped at the user’s request. Test server/log capture stopped,
  temporary keep-awake disabled, media volume remains 0, and TreesInViz remains ON.

## Coverage limits

Pad runtime, 4K, YouTube/WebRTC, physical panel color accuracy, and actual
safe-start UI recovery were not verified. Safe-start gating has automated test
coverage only. HDR whole-page brightness linkage remains a known limitation;
restoration after HDR passed. Performance benefit remains unverified.

Local evidence: `chromium-hmos/artifacts/trees-in-viz-review/`, including
`package-proof.json`, `arkts-focused.log`, `signed-build.log` and pinned upstream
source snapshots. The previous video baseline is in
`chromium-hmos/artifacts/pq-fullscreen-fix/REPORT.md`.
