# Software SDR video underlays and GPU recovery

Implemented for the existing phone overlay path. Pad/2in1 routing and
10-bit/HDR software video are not enabled by this change.

## Confirmed crash

The 2026-11-04 15:29:15 GPU crash in release `ca006b4c` was a SIGTRAP.
The matching ELF has build ID `d86532ddb1e5b1cb084747052cf97188db3eadcc`;
the published symbol archive SHA-256 was verified before symbolization.
PC `0xa90d504` resolves to `OzoneImageBackingFactory::IsSupported`.
Disassembly branches there when a multiplanar format has its external-sampler
bit unset. `SharedImageFormat::ClearPrefersExternalSampler` CHECKs that the bit
was set. Ordinary software YUV can have no such bit.

The shared 154/155 additive patch checks `PrefersExternalSampler()` before
clearing it, both in the factory and the Graphite representation path.
This avoids blaming the unreliable unwound frames mentioning libvulkan.

## Reconnection

The ArkUI content host now survives a GPU disconnect. A new connection may
queue while the old session drains, including before the UI has noticed HUP.
It becomes active only after all old RS leases, producer fences and drain
commands complete. Sequence numbers and current-content references then reset.
Old releases cannot be delivered to the replacement GPU. Detach closes queued
connections and prevents reconnection. Failed recreation retains the existing
fallback to webpage composition; no timeout fabricates buffer releases.

## Software input

`VideoResourceUpdater` opts opaque 8-bit YUV with sRGB/BT.709 primaries and
transfer into its existing RGB conversion/upload path when OHOS scanout is available. External-YUV decoder sampling capability is
not a requirement for this RGBA path, including with Skia Vulkan. Scanout
capability must come from the GPU child’s Ozone instance: the browser instance
intentionally reports no overlays, so its GpuHostImpl creation flag is false. The earlier mappable NV12 pool must leave these CPU frames intact; otherwise
it wraps them as hardware frames and bypasses the uploader. Its existing resource pool,
sync tokens and release callbacks remain responsible for recycling.

Only a backing labelled `VideoResourceUpdater` with the precise scanout,
display-read and raster-write usage marks an RGBA/BGRA pixmap as software video.
Each producer write with an exported fence advances an immutable content ID.
A successful Vulkan SYNC_FD export returning fd=-1 also proves completion;
export errors and missing semaphores do not. This distinction is preserved
explicitly rather than inferred from an invalid generic semaphore handle.
See the [Vulkan synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html). Repeated paused-frame reads keep the ID, so geometry-only
updates reuse the retained read even if its acquire fence is exported again.
An allocation rewritten with new pixels receives a different ID.

Software overlay resource locks are deferred until validated RenderService
release messages retire every outstanding read. Keeping the native allocation
alive alone is insufficient: the uploader must also be prevented from rewriting
it while RS reads it. Channel loss quarantines those reads until context loss;
a disconnect does not fabricate successful retirement.

Zero-origin software frames allocate only their visible size, excluding decoder
coded-size padding. Non-zero origins, partial viewport/clip rectangles and
non-full source UVs use webpage composition. On this device the RS RGB route
showed coded padding despite the submitted source crop, so these cases are not
advertised as supported independent layers.

Wire version 2 transports a second native buffer parcel for software video.
Descriptor order is primary parcel, software-video parcel, primary fence,
video fence. Hardware decoder frames retain their original browser token and
metadata path. Both routes use the same presenter, clipping, paired commit,
RS backend and release machinery. The software wrapper is tagged sRGB or
full-range BT.709 to match the uploaded RGB pixels. RGBA8 cannot carry PQ/HDR.

Promotion still requires opaque, supported geometry and an available presenter.
Ineligible candidates are drawn into the webpage. SDR CPU conversion/upload
has a cost; frame drops, CPU/power and scrolling need device measurement before
claiming a performance improvement.

## Validation

- `bash scripts/test-ohos-video-overlay.sh`: ownership/fences, fractional
  clipping, ABI profiles, real socket reconnection, parcel descriptor transfer,
  malformed packets, format CHECK regression, HDR/alpha/capability exclusions
  and software content-version/fence failure tests.
- `bash scripts/build-ohos-overlay-bridge.sh`: compile/link the actual isolated
  NDK bridge, including host, session, transport and RS backend.
- Additive patch forward/reverse checked against pinned 154 and the actual
  pinned 155 migrated sources.
- Chromium 155 incremental build and Adapter CI passed for `57ed6316`.
  Its signed Lumie HAP contains 67 engine library/resource payloads verified
  byte-for-byte against the published HAR (SHA-256
  `8421a4752d64778da332d2940cb337b556672b309ed175e491c5b1379c3d22be`). This package was installed on the Mate 70 Pro+ and passed the startup smoke
  check and the device validation below.
- Latest local overlay regression suite passes, including the completed/error
  Vulkan export cases. The new Vulkan representation/header hunks apply to
  pinned 154 as well; no full 154 build or device result is claimed.

### Device evidence, 2026-11-04

On Mate 70 Pro+ with `ea96e5c9`, Ganesh Vulkan:

- Ordinary SDR VP9 (`VpxVideoDecoder`, opaque BT.709 profile 0) played more
  than 600 frames without the former GPU CHECK crash, but did **not** promote.
  The subsequent `0d51c7de`/`3293eef7` fixes address the mappable-pool bypass
  and browser-sourced false scanout capability. `3293eef7` and `77b7bf3f` then promoted software VP9 only after the
  Vulkan GPU failed scheduling and restarted on Ganesh GL. This is **not** a
  successful Vulkan result. Resource-retirement protection is included in
  `77b7bf3f`. The latest `57ed6316` preserves completed Vulkan fence exports and
  trims software coded padding; its successful device result is recorded below.
- H.264 and HDR10 HEVC promoted to visible, buffered RS video layers. Both
  exceeded 600 frames, passed four position/clip cases, paused movement,
  CSS-blur demotion and re-promotion. GPU crash count remained zero. This is
  presentation-path evidence, not a display luminance/calibration measurement.
- On the earlier `97fd0d09`, an intentional GPU crash reconnected the presenter
  and recreated RS layers. A newly opened H.264 video then passed promotion and
  fallback again without restarting Lumie. The already-playing hardware clip
  hit `PIPELINE_ERROR_DECODE` on context loss and required reloading; seamless
  playback through GPU death is **not** implemented.

### Completed validation of `57ed6316`, 2026-11-04 17:53–17:56 CST

Mate 70 Pro+ remained on Ganesh Vulkan throughout; GPU crash count was zero
before and after. The test required actual visible RS video buffers, advancing
playback beyond the earlier swap timeout, and processed promotion/demotion
messages. All four clips passed:

| Input | Decoder | Frames by end of case |
|---|---|---:|
| SDR VP9 profile 0 | VpxVideoDecoder, software | 713 |
| SDR H.264 | Platform decoder | 623 |
| SDR 10-bit HEVC | Platform decoder | 663 |
| HDR10 HEVC, PQ/BT.2020 | Platform decoder | 702 |

Fractional movement, paused movement, CSS-blur fallback and re-promotion passed.
Software viewport/left clipping correctly used webpage composition and
re-promoted after restoring full visibility; hardware clipping retained its
independent layer. The HDR stream carried PQ/BT.2020 and static HDR metadata.
This confirms the presentation route, not physical panel luminance calibration.

The software VP9 screenshot comparison excluded the moving marker and measured
all 960,000 RGB channel samples in the static 800×400 picture region. Independent
layer and webpage-composited screenshots were identical (maximum difference 0);
the previous right-side coded-padding stripe and squeezed proportions were gone.

Local evidence: `/tmp/lumie-software-video-work/device-57ed/report.json`, its
`presenter.log` and per-clip screenshots; pixel comparison in
`/tmp/lumie-software-video-work/57ed-pixels/pixel-comparison.json` with both
screenshots. Software HDR and general cropped-software overlay promotion remain
unsupported; this validation does not enable Pad/2in1 routing.

Owned fixtures now include SDR VP9 profile 0 in MP4, generated and checked for
BT.709 tags, decoded grayscale values and all 180 frames. Automation uses a
script-free page and requires RS buffers as well as visibility and advancing
frames. Reports/screenshots are collected separately for each clip.

For the ordinary VP9 test, look for `candidate=supported` together with
`source=software-upload`, then confirm the owning Lumie `LumieVideoLayer` is
visible and has a buffer. A candidate log alone does not prove presentation.
Pause/scroll, resume, fullscreen/exit, navigation, and an isolated GPU restart
must preserve controls and restore the layer. Use H264/HEVC as decoder-path
regressions and 10-bit VP9 as a non-promoted software HDR control.
