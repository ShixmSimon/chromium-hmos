# OHOS overlay investigation probes

## Real-video / HDR preparation

The [HDR test pack](../../../docs/test-pages/video-overlay-hdr/README.md) adds generated
AVC SDR8, HEVC SDR10, HEVC HDR10 and VP9 PQ fixtures, a local playback/transition/
web-overlay test page, host browser QA, and a read-only `hdr_buffer_probe.cpp`
helper for future decoder/RS integration. Media verification and host compilation
are distinct from device validation. No production overlay path is enabled by
this pack; real decoder leases, independent video-layer HDR and physical output
remain unverified by the preparation pack alone. The subsequent shared bridge
implementation and actual decoder lease tests are described below.

## Shared bridge and real hardware decoder (2026-11-03)

See [implementation status and limitations](../../../docs/OHOS_VIDEO_OVERLAY.md#shared-native-implementation-and-real-decoder-results).
`real_video_harness.cpp` drives the shared C bridge with actual AVCodec output;
`real-video-harness.patch` contains temporary Lumie debug hooks, including local
checkout paths that must be adjusted. It runs only on an explicit `lumieRealVideo`
Want, not on normal startup. `lumieRealVideoStop=true` requests asynchronous close.
The caller must keep polling until all leases return; do not destroy by timeout.

Build the standalone module with `scripts/build-ohos-overlay-bridge.sh`. For the
phone harness, apply its patch in a disposable Lumie copy, build/install through
DevEco CLI, generate the fixtures, serve `docs/test-pages` on port 18880, and map
`hdc -t DEVICE rport tcp:18880 tcp:18880`. Then run:

```sh
python3 scripts/tests/ohos-video-overlay/real_video_device_test.py \
  --device DEVICE --output /tmp/real-video-new-run
```

The output directory must not already exist. This runs SDR8/SDR10/HDR10, repeated
SDR/HDR host recreation, interrupted HDR and SDR recovery. It does not enable
browser overlays, test live decoder reconfiguration, or measure display luminance.
Afterwards remove the temporary hooks, rebuild/reinstall the normal HAP, check
that no harness exports remain in it (the new production engine may itself ship
`libohos_video_overlay.so`), and remove the
reverse mapping with `hdc -t DEVICE fport rm tcp:18880 tcp:18880`.

Host lifecycle regression test:

```sh
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -Ioverlay \
  scripts/tests/ohos-video-overlay/session_test.cc \
  overlay/ui/ozone/platform/ohos/ohos_overlay_session.cc -o /tmp/overlay-session-test
/tmp/overlay-session-test
```

Run both lifecycle/backpressure and fractional clipping regressions (ASan/UBSan)
with `bash scripts/test-ohos-video-overlay.sh`. The existing Adapter CI runs
these checks; passing them is not a device playback result.

## RenderService visual and callback probe (2026-11-03)

`rs_api_probe.c`, `rs_buffer_callback.cpp` and `gpu_fence_probe.c` exercise two **visible**
app-owned surfaces, paired buffer submissions, transaction-completion callbacks,
and allocation-specific release fences. On the tested Mate 70 Pro+, both layers
displayed and moved; hide/restore passed; five transaction markers completed;
eight superseded buffers returned release callbacks (six real FDs polled
signaled, two hidden-frame returns carried `-1`).
This supersedes the earlier symbol-only result for this exact firmware.

They are firmware-pinned, AArch64-only debug experiments. C++ callback compilation
requires `-std=c++17 -fno-exceptions -fno-rtti -D_LIBCPP_ABI_NAMESPACE=__h`; link
`native_window`, `native_buffer`, `vulkan`, `hilog_ndk.z`, `ace_napi.z`, and `dl`. The system
uses a different C++ ABI namespace from the application NDK. Selected function
fingerprints and layouts are checked before private factory calls. Do not remove
these checks to make the probe run on an unknown firmware.

`lumie-debug-harness.patch` records the temporary integration against Lumie's
current shell. Its CMake source paths refer to this checkout and need adjustment
on another host. Apply only in a disposable debug working copy, after checking
the patch against local changes. It temporarily auto-runs the inventory/create/
initial phase-50 paint sequence eight seconds after window creation and keeps the screen
on. **Remove the patch and reinstall the regular HAP after testing.** No probe is
intended to remain in Lumie's normal build.

After startup, invoke EntryAbility with integer Want parameter `lumieRsShow`:

| Phase | Operation |
| --- | --- |
| 50 | Full 320x180 textures scaled to the blue/yellow destination rectangles |
| 51 | Both layers move by `(100,200)` |
| 52 | Hide both layers |
| 53 | Show at moved positions |
| 54 | Hide again |
| 60 | Ready green webpage + GPU-written magenta video with a pending 500 ms fence |
| 61 | Same delayed-write test, moving both layers by `(100,200)` |
| 62 | Inject second-layer preparation failure; stage no commands and preserve old pair |
| 63 | Same delayed-write test, moving and shrinking both layers to 75% |
| 66 | Delayed GPU submission, immediate close attempt, then one 100 ms main-thread retry |
| 69 | Export a pending fence from an empty GPU submission; no RS batch |
| 70 | After buffer marker acknowledgment: hide, detach, clean and flush |
| 71 | Poll producer/release fences and unreference server-released allocations |
| 72 | Drain; destroy nodes only when detached and no allocation remains pending |
| 74 | Notify both detached producer surfaces of background; flush |
| 75 | Query live buffer presentation delay/timestamp (currently returns errors) |
| 77 | Marker-gated close sequence: 70, 74, 72; may remain pending |
| 4 | Poll remaining release fences, hide layers, restore app canvas, clear keep-screen-on |

Phases 50–54 each submit two allocations, stage geometry/visibility and add a
transaction marker before one context flush. The probe uses at most twelve native-buffer callback slots. Explicit lifetime
phases can retire allocations and destroy/recreate nodes, but slots are not
recycled; restart before exhausting the twelve slots or 64 transaction markers. Phase 4 does not submit another allocation. Earlier phases
0–43 retain comparison cases used to isolate hardware enablement, clipping and
double scaling; use 50–54 for the corrected path.

Example (substitute the connected device serial):

```sh
hdc -t DEVICE shell aa start -b com.miramira8295.lumiebrowser -a EntryAbility --pi lumieRsShow 50
devecocli ui screenshot --device DEVICE --path /tmp/overlay-phase50.png
```

Capture each phase before advancing and collect the `LumieRsProbe` log. Normal
app startup alone is not this test. Source/log hashes and screenshot paths are in
`device-evidence.json`; the final process's relevant log is retained alongside it.
In the captured run, `show_submitted visibility` mistakenly printed `1` for
phases 52/54; the actual SetVisible calls, screenshots and phase 4 show them
hidden. This diagnostic-only field is corrected in the retained C source, and
the tested-source hash is preserved separately.

The later delayed-write test supplies a real GPU SYNC_FD despite `/dev/sw_sync`
being absent. The video allocation starts red, then the gated GPU writes magenta.
Fixed/moved/resized recordings contained only old blue/yellow and new
green/magenta pairs, with no sampled red sentinel or mixed pair. This is recorded
output evidence, not proof of physical scanout atomicity. The transaction
callback arrived about 492 ms BEFORE GPU completion and must not be used as
presentation feedback. A phase 62 local preparation abort preserved the old pair;
phase 63 then succeeded. Home/return also preserved the existing pair.

This earlier probe did not prove real decoder buffers/HDR. The later bridge
test above covers real decoder lease reuse and HDR metadata transport; physical
presentation, HDR luminance, server-side rollback and cross-process integration
remain unverified.
The earlier run held its final two allocations until process exit. The later
lifetime run below retires all six submitted allocations explicitly. Chromium
overlays remain disabled.

## Reproduce the delayed-write sequence

Use a fresh probe process, wait for the initial eight-second creation hook, then:

1. Run phase 50 and start a screen recording.
2. Run phase 62; verify the old pair remains with `staged_commands=0` in the log.
3. Run phase 63; verify the exported fence is initially pending and the GPU gate
   opens about 500 ms later. Check both the recorded geometry and colors.
4. Stop recording, test Home/return without a new buffer submission, then phase 4.
5. Remove the harness, rebuild/reinstall the regular app and check that its native
   library and RenderService tree are free of probes.

Phases 60/61 can be tested separately from phase 50. Each successful buffer phase
consumes two of the twelve retained slots; restart the process before exhaustion.
Phase 62 discards its unsubmitted allocation/callback and consumes no slot. GPU
jobs release their device/image resources after completion; submitted native allocations remain held until the explicit lifetime phases
retire them (or process exit). This is not a production lifetime implementation.

For the captured 894x1920 recordings, reproduce the patch classification with:

```sh
python3 scripts/tests/ohos-video-overlay/analyze_recording.py RECORDING.mp4 \
  --point web:170:475 --point video:340:611 --point old_corner:102:340 \
  --output /tmp/overlay-frames.json
```

For fixed geometry use `--point web:102:340 --point video:238:462`. Coordinates
must be reselected if the recording dimensions or test rectangles change. The
script preserves unknown colors and original frame timestamps, reports sample
gaps, and requires only Python's standard library plus ffmpeg/ffprobe. Inspect
transition images too; this is a sampled-color diagnostic, not an atomicity test
oracle. Raw recordings/screenshots remain local under
`/tmp/lumie-video-overlay/fence-validation/`; hashes and compact summaries are in
`device-evidence.json`. `rs-fence-run-2026-11-03.log` contains only relevant events
from the two GPU-write test processes.

## Ordered lifetime sequence

The current harness starts directly with phase 50, avoiding legacy NativeWindow
paint references. After the initial visible pair:

1. Phase 66 submits the 500 ms GPU frame and requests teardown immediately. Expect
   `detach_deferred` until its buffer transaction marker completes. The harness
   retries phase 77 once on the owning main thread after 100 ms; this is a retry,
   never a substitute for marker/fence checks.
2. Expect `producer_pending` / `drain_pending`, then the matching release callbacks.
   Invoke phase 72 again after callbacks arrive. Expect zero pending allocations,
   both `node_owner_dropped` counts = 0, and `destroy_submitted`.
3. Check the RenderService tree for absence of both probe nodes. Invoke the
   existing `lumieRsCreate=true` Want, followed by phase 50, and visually inspect
   the recreated pair in the same PID.
4. For ready buffers, run phases 70, 74, then 72. If a result is pending, retain
   resources and retry after the required callback/fence; never bypass the check.
5. Phase 4 clears the harness's keep-screen-on even after native nodes are gone.
   Restore the regular source and HAP.

`rs-lifetime-run-2026-11-03.log` records the failed close-order experiment and the
corrected run. The corrected PID 47475 retires six allocations (three real release
FDs, three server `-1` returns) and destroys two generations. A routing mutex keeps
node destruction from racing the rest of a system ReleaseBuffer call. A separate
producer-fence duplicate prevents retiring an allocation while GPU writes remain
pending. Cleanup must wait for its exact submission marker: immediate CleanBuffer
can cancel queued commands without producing the expected per-buffer callbacks.

Phase 75 returned `40602000` and no value for both queried timestamp types; do not
interpret that error as a presentation time or a universal device limitation.
Node destruction uses the pinned system shared-control ABI, not a direct call to
RSSurfaceNode's destructor. App context/listener refs and callback slots are still
bounded debug state and must be redesigned before production integration.

## Public queue probe

`queue_probe.cc` is a debug-only NAPI test entry (`ProbeOverlay`). It exercises
public NativeImage/NativeWindow/NativeBuffer APIs without Chromium or a graphics
backend. Link it into a debug NAPI test harness with `native_image`,
`native_window`, `native_buffer`, `hilog_ndk.z`, and `ace_napi.z`; C++ exceptions
must be enabled. Register `ProbeOverlay` as a method and call it explicitly after
application startup. It returns the number of failed cases and logs to
`LumieOverlayProbe`.

There are six cases: RGBA8888, NV12 and P010, each with unchanged target geometry
and with a 64x64 -> 128x64 target request change. The two ConsumerSurfaces are
owned by the test. No EGL/Vulkan initialization, pixel copy, codec, RenderService
surface, or visible video is involved. The target must receive the same native
allocation. With unchanged configuration, requesting the released target buffer
must return that allocation. With changed geometry the experiment expects a new
allocation, demonstrating why RequestBuffer success alone cannot retire a
particular decoder lease.

The test explicitly releases its target consumer buffer before requesting it
again. A real overlay presenter cannot make that call: RenderService owns its
consumer. It must observe the actual buffer's release fence/identity. This probe
does **not** establish presentation support, HDR display support, protected video
support, actual codec buffer compatibility, cross-process support, or atomic
multi-layer commits. Do not enable Chromium overlay capabilities from its result.

On the tested retail phone `/data/local/tmp` executables cannot be launched by
hdc shell. Run this through an installed debug HAP, not by trying to bypass the
execution restriction. Do not invoke it automatically during library loading or
before the phone fork server starts.

Compile-only check (macOS SDK location shown):

```sh
/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/llvm/bin/clang++ \
  --target=aarch64-linux-ohos \
  --sysroot=/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot \
  -std=c++17 -Wall -Wextra -Werror -fPIC -fexceptions \
  -c scripts/tests/ohos-video-overlay/queue_probe.cc -o /tmp/ohos-overlay-queue-probe.o
```

Initial device validation: Mate 70 Pro+, 2026-11-02. See
[`docs/OHOS_VIDEO_OVERLAY.md`](../../../docs/OHOS_VIDEO_OVERLAY.md) for the
integration requirements and scope of the results.

## Loaded-library symbol probe

`symbol_probe.c` exports the debug NAPI entry `ProbeOverlaySymbols`. Link it with
`ace_napi.z`, `hilog_ndk.z`, and `dl`; register it only in a temporary debug
harness and invoke it explicitly after application startup. It logs to
`LumieOverlaySymbols` and returns the total number of successful symbol lookups
across scopes (not a capability bitmask or a count of usable APIs).

The probe collects matching already-loaded libraries with `dl_iterate_phdr`,
then uses `dlopen(RTLD_NOLOAD)` and `dlsym` outside the iteration callback. It
does not load additional libraries, read library files, change namespace policy,
call private functions, or dereference their addresses. A failed lookup remains
a failed lookup; do not infer absence of the underlying feature from one symbol
name or scope. The fixed list of seven names covers only simple signatures;
it does not verify `SetBuffer`, callbacks, object layout, or C++ ABI compatibility.

Compile-only check:

```sh
/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/llvm/bin/clang \
  --target=aarch64-linux-ohos \
  --sysroot=/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot \
  -std=c11 -Wall -Wextra -Werror -fPIC \
  -c scripts/tests/ohos-video-overlay/symbol_probe.c -o /tmp/ohos-overlay-symbol-probe.o
```

Device result, 2026-11-02: all seven names resolved through the already-loaded
`librender_service_client.z.so` handle, none through `RTLD_DEFAULT`. Three
matching modules were inspected, without truncation; zero private functions
were called. See `device-evidence.json` for a sanitized result tied to the probe
source hash. The temporary Lumie registration and launch hook were removed,
and the regular application was rebuilt. After the phone reconnected, the
regular HAP was installed and launched successfully (`Smoke: PASS`). The HAP
contains no symbol probe, and all 90 compared kernel/raw-resource entries match
the pre-experiment HAP. This restore check does not validate overlay rendering.

This result must not enable Chromium overlay support: it demonstrates exports,
not working transactions, successful feature enablement, presentation feedback,
buffer retirement, or visual correctness.

## Browser integration checks

See [current integration status](../../../docs/OHOS_VIDEO_OVERLAY.md#chromium-webpage-integration-opt-in-qualification-pending).
`content_host_probe.cpp` is a debug NAPI fixture for an owned ContentSlot host;
it sends a generated cyan RGBA primary over the same wire channel. It is not
a webpage or cross-process decoder test. It requires temporary start/poll/stop
NAPI exports and a page retaining its NodeContent until asynchronous close.

The portable wire test needs Linux (macOS does not support this AF_UNIX
SOCK_SEQPACKET configuration):

```sh
g++ -std=c++17 -Wall -Wextra -Werror -Ioverlay \
  scripts/tests/ohos-video-overlay/transport_test.cc \
  overlay/ui/ozone/platform/ohos/ohos_overlay_transport.cc -o /tmp/overlay-wire-test
/tmp/overlay-wire-test
```

A production build must package the new bridge and opt in through
`AuraStartupConfig.enableVideoOverlays`. Validate actual webpage videos,
including paused scrolling, promotion/demotion, HDR-to-SDR changes and process
loss, independently for GL/Vulkan/Graphite. Host checks alone must not change
the default activation flag.

`webpage_promotion_validation.mjs` uses the minimal `promotion.html` fixture.
It requires 360 additional video frames and visible `LumieWebLayer` /
`LumieVideoLayer` records, checks fractional geometry and viewport clipping,
checks paused repositioning, forces demotion with a
CSS filter, and verifies re-promotion for SDR8, SDR10 and HDR10. It also requires
presenter command-processing logs; command processing is not physical display
timing or proof of HDR luminance. Run separately for each backend:

```sh
node scripts/tests/ohos-video-overlay/webpage_promotion_validation.mjs \
  http://127.0.0.1:19422 \
  http://127.0.0.1:18880/video-overlay-hdr/promotion.html \
  /tmp/lumie-webpage-promotion-gl DEVICE_SERIAL
```

The output directory must be new. Only the created tab is closed. The test
retains filtered test-layer records and presenter logs, not other windows'
trees. Screenshots, scroll/fullscreen, navigation/background and GPU-loss
checks remain separate qualification steps. Syntax checking the script is
not a device result.
