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
Each fenced producer write advances an immutable content ID. Missing fences
disable promotion. Repeated paused-frame reads keep the ID, so geometry-only
updates reuse the retained read even if its acquire fence is exported again.
An allocation rewritten with new pixels receives a different ID.

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
- Complete kernel compilation and webpage promotion require CI/device checks.

For the ordinary VP9 test, look for `candidate=supported` together with
`source=software-upload`, then confirm the owning Lumie `LumieVideoLayer` is
visible and has a buffer. A candidate log alone does not prove presentation.
Pause/scroll, resume, fullscreen/exit, navigation, and an isolated GPU restart
must preserve controls and restore the layer. Use H264/HEVC as decoder-path
regressions and 10-bit VP9 as a non-promoted software HDR control.
