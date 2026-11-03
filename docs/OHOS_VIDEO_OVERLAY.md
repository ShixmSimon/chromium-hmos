# OHOS video overlay: transaction investigation and device probes

Status (2026-11-03): **Chromium webpage promotion is now wired in source behind
`AuraStartupConfig.enableVideoOverlays: true` (default off).** One Ozone presenter
serves GL, Vulkan and Graphite initialization, with a browser-owned ContentSlot
host and a dedicated GPU socket. Full Chromium compilation and end-to-end
webpage validation are still pending; the earlier real-decoder results below
validate the shared RS mechanism, not this new browser integration. The user has
accepted the visible brightness; this work does not change HDR luminance policy.

Earlier experiments: two app-owned RenderService layers visibly render, move,
hide and reappear on the retail phone; transaction callbacks and allocation-specific
release notifications have passed (six signaled FDs, two no-fence returns).
A subsequent real GPU delayed-write test recorded paired updates during fixed,
moved and resized geometry, with no sampled unfinished/mixed frame. Local
preparation failure and background/foreground behavior also passed. The later
lifetime run retires six allocations across two node generations and verifies
destruction/recreation, including closure while GPU work is pending. Physical
scanout timestamps and real browser integration remain unverified.
The real overlay manager now replaces `StubOverlayManager`. It only accepts
candidates while a presenter is connected to an explicitly enabled host; absent
hosts and unsupported firmware retain the existing output path. The legacy
`OhosNativePixmap::ScheduleOverlayPlane()` entry is not used by this presenter.

## Reuse boundary

Pinned Chromium 155 revision: `f043649a0ef1fd96e48d56ddbfc8a4a99aa2ca37`.

Reuse the Ozone path rather than copying Android's MediaCodec/SurfaceControl
backend. `OverlayCandidateFactory`, `OverlayProcessorOzone`, and
`OverlayStrategyUnderlay` already handle candidate selection and webpage holes.
`SkiaOutputDeviceBufferQueue` and `OutputPresenterGL` can call one `gl::Presenter`
from the GL and Vulkan initialization paths. The historical `GL` name does not
make this a GL-only presentation abstraction. Graphite's current OHOS output
path and phone fork image allocation/import must be checked separately before
claiming support for every backend.

The integration must cover more than replacing the overlay oracle:

1. A shared OHOS presenter with an actual multi-layer commit and completion
   contract; primary webpage buffers must participate in that contract too.
2. A real Ozone overlay oracle, advertising only the verified presenter's formats,
   transforms, crop behavior, alpha, HDR and synchronization capabilities.
3. SharedImage overlay access, fences and native pixmap lifetime across display
   reads, including the phone browser/fork-GPU IPC boundary.
4. A layer host attached to each browser window, with viewport coordinates,
   transparent webpage output, video underlay geometry, visibility, and lifecycle.
5. Demotion, navigation, resizing, pause/resume and crash recovery that never
   return a displayed frame to its decoder early.

Android's video Surface chooser is useful architectural reference, but its
legacy direct-Surface route and newer compositor-driven SurfaceControl route
must not be conflated. Existing OHOS decoder ConsumerSurfaces are not display
layers.

## Device experiment

Source: [`queue_probe.cc`](../scripts/tests/ohos-video-overlay/queue_probe.cc).
It links only public NDK APIs and the debug NAPI/logging harness, not Chromium,
EGL or Vulkan. Tested inside a signed Lumie debug HAP on a Mate 70 Pro+; a shell
executable was rejected by the device, so no execution restriction was bypassed.
The temporary Lumie hook is not intended to ship.

Each test creates two app-owned ConsumerSurfaces. It requests and flushes a
buffer on the source, acquires its consumer lease, attaches that same allocation
to the destination queue, flushes/acquires/releases it there, requests the
released destination buffer, waits the returned fence and detaches it. The
source lease remains held until the destination queue is torn down.

Observed on 2026-11-02:

| Native format | Fixed 64x64 destination | Destination request changed to 128x64 |
| --- | --- | --- |
| RGBA8888 (12) | Same allocation received and returned | A different allocation returned |
| NV12 (24) | Same allocation received and returned | A different allocation returned |
| P010 (35) | Same allocation received and returned | A different allocation returned |

There is no CPU pixel copy in this path. These are allocated test buffers, not
actual codec/HEBC/protected frames. P010 allocation transfer does not demonstrate
HDR display or metadata correctness. Both consumers are controlled by the test;
there is no RenderService presentation, webpage hole or cross-process transfer.
A passing probe must not enable production overlay support.

## Release and commit constraints

`NativeWindowFlushBuffer()` acknowledges enqueue, not completion of display
reads. Retain the NativePixmap/decoder lease until the actual destination
allocation is returned and its release fence has completed. A generic swap
callback, another buffer being requested, a delay of a few vsyncs, or the next
frame being flushed is not a release acknowledgement for that allocation.

The resize experiment is particularly important: the destination can replace a
released allocation during `RequestBuffer()`. Its returned buffer/fence then
cannot be used as a release event for an arbitrary old allocation. Never discard
all outstanding decoder leases after observing such a request. A future backend
must have a verified drain/retirement protocol that also handles pause, hidden
surfaces and consumer destruction.

Chromium's `gl::Presenter::ScheduleOverlayPlane()` explicitly stages planes for
an atomic `Present()`. In the installed public SDK, XComponent/SurfaceHolder
provide individual surfaces and geometry updates, while NativeWindow provides
per-window buffer submission. No public SurfaceControl-equivalent transaction
or per-buffer producer release callback has been identified in the inspected
headers/docs. This is an unresolved integration requirement, not proof that
HarmonyOS itself lacks the capability. Setting equal desired-present timestamps
on two windows does not establish an atomic commit contract.

Do not enable the overlay oracle and make separate immediate FlushBuffer calls
while this remains unresolved: Chromium may remove the video quad from the
webpage before a correctly positioned video layer can replace it.

## System-side transaction path traced

Pinned OpenHarmony `graphic_graphic_2d` revision:
`885442df449fb24ae51de1018b18f652b5133971`. This is an implementation reference,
not a claim that the retail HarmonyOS firmware contains that exact revision.

`RSSurfaceNode` exposes `SetDelegateMode`, `SetBuffer` (with an acquire fence and
buffer release-fence callback), `SetDelegateSrcRect`, and `SetDelegateDstRect`.
Its delegate buffer manager enqueues buffer and rectangle commands through an
`RSTransactionHandler`. Release tracking identifies the allocation by buffer
sequence; a transaction-completion notification is a different callback.

The server's delegate compositor groups commands by sending process, transaction
token and index. `RSMainThread` processes the queued batch before consuming and
updating surface nodes at the start of a render frame. This is concrete evidence
of a relevant batching mechanism, beyond the existence of methods named
Begin/Commit. It is **not yet a verified atomic presentation contract**: buffer
enqueue failures and acquire-fence readiness still require device experiments.
`NotifySurfaceTransactionListener` runs during the main-thread frame loop; its
timestamp must not be reported as proven physical display completion. Keep
command acceptance, presentation feedback, and allocation release distinct.

Permission checks in the inspected upstream implementation do not simply forbid
all ordinary applications. The access verifier allows the relevant transaction
and listener operations; `SetDelegateMode` verifies that the caller owns the
node (`ExtractPid(nodeId) == remotePid`, and the supplied PID must match).
Listener registration also checks process identity. This makes node ownership
and the browser/GPU process boundary part of the design; it does not establish
the permissions of every retail firmware.

There are additional constraints:

- Server operations are compiled behind `RS_ENABLE_DELEGATE_COMPOSITE` and can
  return false when disabled. Exported client symbols do not prove this feature
  is enabled in the server.
- Listener commands are associated with the creating thread. Send a complete
  frame batch through the same verified transaction context/thread.
- The inspected render-node implementation does not undo delegate mode when
  passed false. Do not assume that toggling the flag implements demotion.
- A buffer-manager destructor can invoke callbacks with `-1`; teardown needs its
  own verified retirement protocol, not an assumption that every such callback
  proves a displayed frame is now safe to overwrite.
- These are internal C++ APIs, absent from the public NDK headers inspected.
  `sptr`, `UniqueFd`, callback types, node construction and destruction all need
  ABI verification. Matching a symbol name is insufficient to call it safely.

There is also a concrete upstream Web-component integration example. At
`arkui_ace_engine` revision `21b150e7bb1d5308846f8e5be8ef0482fb433315`,
`WebDelegate::InitWebViewWithWindow` and `InitWebViewWithSurface` pass
`SetParamsForDelegateCompositing(surfaceNodeId, rsHandle, uiContextToken)` to
NWeb initialization. The handle is obtained from the owning surface node's
`RSUIContext::GetConnectToRender()`, and the token from that same context.
The handle is a retained in-process remote-object pointer represented as an
integer, **not** an IPC-portable integer handle that can be copied to Lumie's
forked GPU process. An actual process bridge must transfer the appropriate
reference through a supported IPC mechanism.

This upstream path is guarded by the `web.delegatedCompositing.enable` system
parameter, whose source-level fallback is false. That does not reveal the
phone's configured value; the experiment neither read nor changed it. This
example establishes a useful host/kernel integration boundary, not a public API
or a ready-made implementation in this Chromium fork.

Source:
[WebDelegate host/context handoff](https://github.com/openharmony/arkui_ace_engine/blob/21b150e7bb1d5308846f8e5be8ef0482fb433315/frameworks/core/components/web/resource/web_delegate.cpp).

Pinned sources:
[surface node](https://github.com/openharmony/graphic_graphic_2d/blob/885442df449fb24ae51de1018b18f652b5133971/rosen/modules/render_service_client/core/ui/rs_surface_node.cpp),
[delegate manager](https://github.com/openharmony/graphic_graphic_2d/blob/885442df449fb24ae51de1018b18f652b5133971/rosen/modules/render_service_client/core/feature/delegate_composite/rs_delegate_composite_buffer_manager.cpp),
[server batching and release callbacks](https://github.com/openharmony/graphic_graphic_2d/blob/885442df449fb24ae51de1018b18f652b5133971/rosen/modules/render_service_base/src/feature/delegate_composite/rs_delegate_composite_callback_manager.cpp),
[frame processing](https://github.com/openharmony/graphic_graphic_2d/blob/885442df449fb24ae51de1018b18f652b5133971/rosen/modules/render_service/core/pipeline/main_thread/rs_main_thread.cpp),
[connection ownership checks](https://github.com/openharmony/graphic_graphic_2d/blob/885442df449fb24ae51de1018b18f652b5133971/rosen/modules/render_service/core/transaction/rs_client_to_render_connection.cpp),
[IPC access verifier](https://github.com/openharmony/graphic_graphic_2d/blob/885442df449fb24ae51de1018b18f652b5133971/rosen/modules/render_service/core/transaction/rs_iclient_to_render_connection_ipc_interface_code_access_verifier.cpp),
[server feature gate](https://github.com/openharmony/graphic_graphic_2d/blob/885442df449fb24ae51de1018b18f652b5133971/rosen/modules/render_service/core/rs_render_pipeline_agent.cpp).

## Retail-device symbol availability

[`symbol_probe.c`](../scripts/tests/ohos-video-overlay/symbol_probe.c) was run
inside Lumie's signed debug HAP on the same phone on 2026-11-02 at 17:28:42
(device log time). It enumerates only the current process's loaded modules with
`dl_iterate_phdr`, then uses `dlopen(RTLD_NOLOAD)` and `dlsym` through the normal
application linker. It does not load new libraries, read system library files,
change linker namespaces, dereference addresses, or call any private function.

All seven queried symbols were found through the already-loaded
`/system/lib64/librender_service_client.z.so` handle:

| RSSurfaceNode | RSTransaction |
| --- | --- |
| SetDelegateMode | Begin |
| SetDelegateDstRect | Commit |
| SetDelegateSrcRect | FlushImplicitTransaction |
| CleanBuffer | |

`RTLD_DEFAULT` resolved none of them. Three matching loaded modules were
inspected without truncation; seven symbol hits, zero private calls. This proves
availability of those exports to this application's linker lookup, not working
layer creation, server feature enablement, ABI compatibility, or synchronization.
At this initial stage, `SetBuffer` and callback registration were not exercised;
the later experiment below supersedes that limitation for the pinned firmware.
The temporary application hook was removed after the
experiment; the standalone probe source is retained for reproducibility.

## Visible layers and release fences: 2026-11-03

The debug-only `rs_api_probe.c` and `rs_buffer_callback.cpp` were exercised inside
the normal signed Lumie app process on the same Mate 70 Pro+. Both
`RSSurfaceNode::Create` calls succeeded and both `SetDelegateMode(true)` calls
returned true. The process attached two SELF_DRAWING nodes to its own RS root,
submitted separate RGBA8888 NativeBuffers with `SetBuffer`, and flushed both
nodes' commands through the same RSUIContext transaction handler.

Three probe configuration errors were isolated and corrected on-device:

1. `SetHardwareEnabled(true, DEFAULT, true)` is needed in addition to delegate
   mode for these nodes to enter the hardware-node collection. Raising Z alone
   did not fix the missing image. An empty `VisibleRegion` in the dump alone is
   not reliable proof of invisibility: working self-drawing layers also showed it.
2. `SetFrame` must use the same parent-space rectangle as `SetBounds` here.
   Using `(0, 0, width, height)` clipped the offset blue layer and entirely hid
   the yellow layer.
3. `Gravity::TOP_LEFT` avoids applying the default RESIZE transformation on top
   of the delegate source/destination scaling. The final pictures contain all
   eight columns and six rows of each 320x180 test buffer.

Final device sequence, PID 28467, 09:48–09:51 local time:

| Check | Observed result |
| --- | --- |
| Initial geometry | Blue `(100,450,1000,700)`, yellow `(320,650,600,340)`; both visible |
| Move | Both translate by `(100,200)` without changing their relative positions |
| Hide / restore / hide | Both disappear, reappear at moved positions, then disappear again |
| Transaction markers | Sequences 0–4 consumed; five matching completion callbacks |
| Buffer replacement | Ten separate allocations submitted across five paired batches |
| Release identity | Eight superseded allocations matched by node owner and sequence |
| Release synchronization | Six real FDs polled signaled and closed; two hidden-frame returns had fence `-1` |
| Final two allocations | Retained until process teardown; not counted as verified retirement |

`SurfaceTransactionListener` registration succeeded. Its `GetCommand` marker was
added to the same handler before `FlushImplicitTransaction`. The separate
`SurfaceNodeBufferReleaseListener` was connected and registered without replacing
an existing listener; records were forwarded to the matching surface's
`ReleaseBuffer`, which delivered the per-buffer fence callbacks. These are actual
private API calls and server callbacks, not symbol lookup results.

The ABI bridge is deliberately a firmware-pinned experiment: it checks hashes
of selected loaded function bodies, uses AArch64 indirect returns, and builds
callback objects with the system `std::__h` ABI. It retains bounded object and
buffer references until process exit. This is not production lifetime handling
or a portable binary interface. No system library files, namespace policy or
device privilege settings were changed.

The five ready-buffer batches do **not** establish atomic presentation when one
acquire fence is late, nor physical display timestamps. `/dev/sw_sync` was absent
(`ENOENT`) in the app process, so that initial run omitted delayed fences.
The subsequent GPU experiment below supplies a real graphics fence; it does not
substitute an eventfd or timeout. Real codec frames, NV12/P010 display, HDR,
transparent webpage holes, cross-process handoff, allocation reuse and explicit
layer destruction/recreation also remain to be tested.

See [recorded device log](../scripts/tests/ohos-video-overlay/rs-device-run-2026-11-03.log)
and [evidence manifest](../scripts/tests/ohos-video-overlay/device-evidence.json).
Temporary app hooks and keep-screen-on behavior are removed after this run;
the harness patch is retained separately for the next controlled experiment.

## Delayed GPU writes, resize and preparation failure: 2026-11-03

`gpu_fence_probe.c` creates a separate Vulkan device for this debug experiment.
On the Maleoon 920, timeline semaphores, external binary SYNC_FD export and OHOS
NativeBuffer import are available. A host timeline gate postpones actual GPU
writing by 500 ms. The imported RGBA buffer initially contains a **red sentinel**;
a GPU clear changes it to magenta only after the gate opens. The pending exported
fence accompanies that video-layer buffer, while a ready green webpage test
buffer is submitted in the same RenderService batch. The independently running
gate thread always releases the wait. Vulkan here is only the test producer,
not a new dependency or backend choice for the eventual browser presenter.

Three screen recordings were decoded without frame duplication. Color patches
inside both layers, plus an old-position corner for geometry cases, were checked
in every encoded frame and transition images were visually inspected:

| Case | Recorded frames | Old pair / new pair | Last-old to first-new sample gap |
| --- | ---: | ---: | ---: |
| Fixed geometry, phase 60 | 428 | 87 / 341 | 16.500 ms |
| Move `(100,200)`, phase 61 | 122 | 79 / 43 | 18.388 ms |
| Move and resize to 75%, phase 63 | 156 | 107 / 49 | 18.933 ms |

Only blue/yellow and green/magenta pairs were sampled. No red sentinel or mixed
old/new pair was recorded. The old corner disappeared in the same recorded frame
as the moved/resized pair appeared. Final resized rectangles were webpage
`(200,650,750,525)` and video `(420,850,450,255)`. This is positive evidence for
this firmware's recorded output; the system recorder is not physical scanout,
and cannot prove a universal per-vsync atomicity contract.

The final run also provides a decisive **negative** result for presentation
feedback. Monotonic timestamps in PID 59796:

| Event | Nanoseconds |
| --- | ---: |
| Exported fence polled pending | 284480645040497 |
| Paired transaction flushed | 284480645895185 |
| Transaction-completion callback | 284480655888559 |
| GPU gate opened | 284481146343310 |
| GPU queue idle | 284481147594403 |

The callback precedes GPU completion by **491.706 ms**. It acknowledges command
processing, not completed drawing or physical presentation. Do not map it to
Chromium presentation feedback or use it to return decoder leases.

The native probe now prepares both allocations, callbacks and duplicated fence
FDs before staging geometry or buffers. Phase 62 injects failure while preparing
the second layer: zero commands are staged, the first temporary allocation and
callback are discarded, and the old blue/yellow pair remains visible. Phase 63
then succeeds, confirming that abort did not consume the callback slot. This
validates local preflight recovery only; it does not establish server-side
rollback after an enqueue rejection. Later failures after staging commands are
still an unresolved reason not to reuse this probe as a production presenter.

Pressing Home removed the test layers from the desktop. Returning to the same
app process restored the resized pair without submitting new buffers. Phase 4
hid the layers and disabled temporary keep-screen-on. Explicit in-process node
destruction/recreation and retirement of the final two allocations remain
unverified; process teardown is still the experiment's final cleanup mechanism.

All three native sources pass `-Wall -Wextra -Werror` compilation; the temporary
HAP built, installed and started successfully. The four Lumie hooks were then
restored, the normal HAP rebuilt and installed (`Smoke: PASS`). Its prewarm
library contains neither RS nor GPU probes, all 90 compared kernel/raw entries
match the pre-experiment HAP, and the RenderService tree contains no test nodes.
The normal webpage was visually checked after restoration.

Reproduction and per-recording hashes are in
[the probe README](../scripts/tests/ohos-video-overlay/README.md),
[the filtered fence log](../scripts/tests/ohos-video-overlay/rs-fence-run-2026-11-03.log)
and [the evidence manifest](../scripts/tests/ohos-video-overlay/device-evidence.json).
The initial fixed/move runs precede the paired-preflight refactor; the final
resize/failure run exercises the exact retained native sources.

## Final-buffer retirement and node recreation: 2026-11-03

The lifetime probe adds a verified close sequence on the same retail firmware:

1. Wait for the **specific last buffer transaction marker** to complete. This
   acknowledges command processing only; it is not a presentation timestamp.
2. Hide and remove both nodes from their parent, invoke `CleanBuffer(true)`, and
   flush through the owning main-thread context.
3. Invoke `ProducerSurface::GoBackground()` for each test surface, then flush.
   `CleanBuffer` plus detach alone left both final allocations outstanding.
4. Retire each native allocation only after its matching **server** release
   notification, its release fence, and its retained producer/acquire fence are
   all complete. Serialize retirement against the entire system `ReleaseBuffer`
   call, since it accesses the node after invoking the nested buffer callback.
5. Remove the allocation from callback routing, drop the probe's native reference,
   then release the node's last system shared owner. Flush the destructor's
   commands. Do not directly call the node destructor or treat its fallback `-1`
   callbacks as server release acknowledgements.

Two failures established why these steps are necessary:

- PID 23908: detach + `CleanBuffer` left `drain_pending=2`.
- PID 44756: cleaning immediately after flush could cancel the still-unprocessed
  buffer transaction; only the old pair returned, leaving the new pair pending
  even after GPU completion. The probe kept those allocations until process exit.
  The final code defers cleanup until the corresponding marker is acknowledged.

Final exact-source run, PID 47475, 11:29–11:31:

| Check | Result |
| --- | --- |
| Close requested immediately after delayed-GPU submission | Deferred for transaction marker 1 |
| Retry after marker completion | Both surfaces entered background successfully |
| GPU still writing at retry | `producer_pending slot=3`; destruction remained blocked |
| Release callbacks | Six unique submitted allocations returned across two generations |
| Release fences | Three real FDs polled ready and closed; three server returns carried `-1` |
| Native lifetime | All six probe references explicitly unreferenced after synchronization |
| Node lifetime | Four last-owner releases, each previous owner count minus one = 0 |
| First destruction | RenderService dump contained zero `LumieProbe` nodes |
| Recreate without restarting process | Both new layers visibly rendered; generation 2 then drained and destroyed |

For the in-flight frame, the acquire fence was exported pending at monotonic
`287293186099561 ns`; the close retry still saw it pending at
`287293289816749 ns`. The GPU gate opened at `287293686038467 ns` and the queue
became idle at `287293686735811 ns`. The video release callback arrived later
with a real FD. Neither a fixed delay nor command completion substitutes for
those fences. The harness's 100 ms timer merely retries the close attempt;
the native code checks the marker and fences again and can remain pending.

The lifetime ABI remains firmware-pinned. The loaded getter, clean, remove,
background and destructor functions are fingerprinted. Last-owner release uses
the verified system `std::__h` control-block layout and system weak-release
function, rather than constructing a default-NDK C++ owner. Temporary
`GetSurface()` sptr references are balanced through the system's non-const
`RefBase::DecStrongRef`. Window/context/listener references and callback slots
remain bounded debug-process state; this is not a portable production bridge.

Presentation feedback remains **unknown**. On both live layers,
`ProducerSurface::GetPresentTimestamp(sequence, type, value)` returned
`rc=40602000`, leaving `value=-1`, for both delay and timestamp types. This proves
only that this query did not produce usable feedback for these delegated
buffers. It does not establish that the device has no presentation capability.
A future presenter must use Chromium's unavailable/failure presentation feedback
until a valid source is established, while tracking swap acceptance and buffer
retirement separately. Do not fabricate a timestamp from `Now()` or the marker.

The signed experimental HAP and native warnings-as-errors checks passed. The
normal source and HAP were restored after testing; the same 90 kernel/raw-resource
entries remain unchanged. See the
[lifetime device log](../scripts/tests/ohos-video-overlay/rs-lifetime-run-2026-11-03.log)
and [evidence manifest](../scripts/tests/ohos-video-overlay/device-evidence.json).
No real decoder lease, cross-process frame, HDR or browser overlay is tested here.

## Concrete implementation route and remaining gates

Continue with a small RenderService compatibility bridge before changing
Chromium's overlay capabilities. Keep it disabled unless its exact platform
contract is validated; retain the existing compositor fallback.

1. Extract the tested creation, batch submission, release routing and ordered
   drain/destroy/recreate sequence into an owned compatibility bridge. Add a
   fail-closed capability probe for each supported firmware; replace the remaining
   bounded debug listener/context state with production lifetimes.
2. Use one owning process and transaction thread for the layer host. A browser
   process bridge is the initial design to investigate: receive GPU frame buffers
   and fences over IPC, and submit webpage buffer, video buffer, crop, destination
   rectangle and visibility together. Do not independently flush the webpage
   through the current NativeWindow path and assume it joined the video batch.
3. Extend the passing delayed-write and ordered teardown tests to actual decoder
   leases, server enqueue rejection, process loss and repeated allocation reuse.
   Use unavailable presentation feedback until a trustworthy source is verified.
   A failed enqueue must not leave a webpage hole with no matching video layer.
4. Only after these gates pass, wire the shared presenter, overlay oracle,
   SharedImage access and promotion/demotion paths into Chromium.

Once the contract is proven, implement one presenter and backend-specific image
and fence adapters, then validate fullscreen transitions, scrolling, subtitles,
danmakus, CSS effects, HDR/SDR switches, navigation and GPU recovery. Default
integration remains Chromium 155; synchronize applicable changes to 154 later.

## Prepared real-video / HDR test pack (2026-11-03)

The [test pack](test-pages/video-overlay-hdr/README.md) now contains generated
SDR8 AVC, SDR10 HEVC, HDR10 HEVC and PQ VP9 clips, a mobile test page for DOM
subtitles/occlusion/layout and SDR-HDR-SDR transitions, plus a read-only native
buffer inspection helper. Full media decoding, code values, stream tags and
HDR10 SEI were checked on the host; browser smoke checks and arm64 helper
compilation passed. [Preparation evidence](../scripts/tests/ohos-video-overlay/hdr-preparation-evidence.json)
records hashes and scope. The helper is not yet wired into decoder/RS submission.
This is preparation only; it does not extend the earlier device claims to real
video, decoder lease reuse or HDR display. The ordinary Lumie installation and
Chromium overlay capability remain unchanged by this preparation.

The subsequent 2026-11-03 [device baseline](test-pages/video-overlay-hdr/README.md#2026-11-03-真机基线结果)
played all four clips under the installed 155 OpenGLES/GaneshGL path. HEVC HDR10
decoded with PQ/BT.2020 metadata intact, but display capability remained false
and the app output Surface queue reported HDR=0 with no static HDR metadata.
VP9 profile 2 used software decoding. This is an observed SDR output baseline,
not validation of the still-unimplemented independent video presenter/HDR path.

## Primary sources inspected

- [Chromium Presenter contract](https://chromium.googlesource.com/chromium/src/+/f043649a0ef1fd96e48d56ddbfc8a4a99aa2ca37/ui/gl/presenter.h)
- [Chromium Ozone overlay processor](https://chromium.googlesource.com/chromium/src/+/f043649a0ef1fd96e48d56ddbfc8a4a99aa2ca37/components/viz/service/display/overlay_processor_ozone.cc)
- [Chromium output initialization](https://chromium.googlesource.com/chromium/src/+/f043649a0ef1fd96e48d56ddbfc8a4a99aa2ca37/components/viz/service/display_embedder/skia_output_surface_impl_on_gpu.cc)
- Installed public SDK: `native_window/external_window.h`,
  `native_image/native_image.h`, `ace/xcomponent/native_interface_xcomponent.h`,
  `arkui/native_render.h`, and XComponent/RenderNode ArkTS declarations.
- `devecocli docs`: XComponent API reference; NativeImage/NativeWindow API
  references; 高效利用HWC的低功耗设计; 自渲染图层未使用硬件合成问题分析.
- [OpenHarmony native window implementation](https://github.com/openharmony/graphic_graphic_surface/blob/master/surface/src/native_window.cpp)
  and [buffer queue implementation](https://github.com/openharmony/graphic_graphic_surface/blob/master/surface/src/buffer_queue.cpp)
  (implementation reference only, not a guaranteed ABI for the tested firmware).


## Shared native implementation and real decoder results

The implementation is in `overlay/ui/ozone/platform/ohos/ohos_overlay_session.*`,
`ohos_rs_overlay_backend.*`, and `ohos_overlay_bridge.*`. It is deliberately a
separate NDK library with a C interface; do not compile its system `__h` callable
objects into Chromium's `__Cr` ABI. Build with
`scripts/build-ohos-overlay-bridge.sh /tmp/libohos_video_overlay.so`.
The results in this section predate the browser integration described below;
the recorded source hashes identify that earlier test build.

Implemented contracts:

- One owning UI transaction thread, explicit context token and parent ID, and
  two nodes. Known private entry points/layouts are fingerprinted; an unknown
  firmware/context fails creation. This is a compatibility boundary for the
  tested firmware, not a public SDK guarantee for all HarmonyOS devices.
- Prepare both planes before staging commands, hold their actual read leases,
  duplicate acquire fences, and submit buffers/geometry/visibility in one flush.
  No video CPU mapping or pixel copy is used by the bridge.
- Dynamic allocation routing, bounded frames in flight, backpressure, and
  retirement only after server release, release-fence completion AND producer
  completion. The recorded node ID must resolve to the exact owned node.
  `client_pid` in delegate release records is **0** on this device; filtering it
  against the app PID stalls after a few frames and was corrected.
- Close waits for the final command marker, hides/detaches/cleans the nodes,
  flushes, calls producer `GoBackground`, and waits for final releases before
  destroying node owners. No timeout manufactures a release. A fatal command
  setup invariant aborts rather than handing in-use buffers back to a decoder.
- NativeBuffer format/color/HDR metadata are read, not fabricated or rewritten.
  Primary stays SDR RGBA/BGRA; video supports NV12 SDR or P010 SDR/PQ. HLG and
  unknown formats/color spaces fail eligibility. Equivalent SDK color enums
  (`SRGB_FULL` read back as `DISPLAY_SRGB`) are handled.
- This firmware cannot read the NONE HDR type through the public getter on a
  fresh SDR allocation (`50102000`). Only an explicit producer attestation that
  the allocation/decoder queue has **never** carried HDR permits this case;
  unexpected errors and stale static metadata reject the frame. Do not set this
  flag on arbitrary imported buffers or reuse it across a live HDR-to-SDR queue.

`real_video_harness.cpp` is a separate debug driver: AVSource/Demuxer → hardware
AVCodec → ConsumerSurface lease → the same bridge. The primary is a generated
SDR transparent layer with an opaque border/line. It is not a webpage, browser
promotion test, or an audio/video synchronization implementation. Its allocation
and UI timer pacing are not a playback performance benchmark.

On the Mate 70 Pro+ API 26 retail firmware, in one process:

| Sequence | Submitted / returned decoder leases | After close |
| --- | --- | --- |
| AVC SDR8 | 180 / 180 | 0 pending, no test nodes |
| HEVC SDR10 | 180 / 180 | 0 pending, no test nodes |
| HEVC HDR10 | 180 / 180 | 0 pending, no test nodes |
| AVC SDR8 after HDR | 180 / 180 | 0 pending, no test nodes |
| HEVC HDR10 recreated | 180 / 180 | 0 pending, no test nodes |
| HDR10 interrupted | 35 / 35 | 0 pending, no test nodes |
| AVC SDR8 after interruption | 180 / 180 | 0 pending, no test nodes |

HDR video was P010 (format 35), native BT.2020/PQ limited (enum 10), HDR10 type
(public enum 1), mastering 0.0001–1000 nit, MaxCLL 1000 / MaxFALL 198. The RS
video queue retained the same 48 metadata bytes and internal HDR10 type 2;
primary metadata stayed empty/type 0. The queue's separate legacy `HDR` field
still printed `0`: metadata arrival and visible output do not establish panel
HDR luminance. No physical HDR output or presentation-timestamp success is
claimed. Switching here destroys/drains the codec queue and host between files;
live adaptive stream reconfiguration remains untested. VP9 software decode is
also outside this hardware-only driver.

Evidence: [results and source hashes](../scripts/tests/ohos-video-overlay/real-video-evidence.json),
[filtered decoder/frame log](../scripts/tests/ohos-video-overlay/real-video-device-2026-11-03.log),
[RS video queue](../scripts/tests/ohos-video-overlay/real-video-hdr-video-surface.txt),
[RS primary queue](../scripts/tests/ohos-video-overlay/real-video-hdr-primary-surface.txt).
The temporary Lumie hooks are retained as `real-video-harness.patch`; remove them
and reinstall the regular HAP after tests. `real_video_device_test.py` reproduces
the sequence against that debug build and saves only these test-layer records.

Host ASan/UBSan tests exercise rejected geometry/hole/color combinations,
second-plane preparation failure, frame pressure, wrong/stale release events,
pending fences, ordered asynchronous close, and 500 recycled allocation uses.
The standalone ARM64 library and temporary phone HAP compile successfully.

## Chromium webpage integration (opt-in, qualification pending)

The code now connects these stages:

1. `ohos_video_decoder` registers each original browser-process decoder read
   with an unpredictable acquisition token. NativePixmapHandle/Mojo carries the
   token to the fork GPU. Only registered reads receive SCANOUT usage.
2. The Ozone candidate oracle requires that original NativePixmap/token, an
   opaque NV12 or P010 video, supported PQ/SDR color, integral geometry, identity
   transform, no rounded corners/effects and a contained clip. Chromium's
   underlay strategy creates the webpage hole; ineligible videos stay composed.
3. `ohos_overlay_presenter` submits the SDR RGBA/BGRA primary and optional video
   together. A dedicated nonblocking SOCK_SEQPACKET transports the primary's
   parcel handles, per-plane acquire fences and geometry. Video tokens resolve
   to the original read/metadata in the browser; no pixels or C++ pointers cross
   the GPU channel. Missing planes reject the whole frame, never just the video.
4. `ohos_overlay_host` attaches two RS layers beneath an owned native RenderNode
   in the WebWindow ContentSlot. The shell owns position/clipping/stacking;
   buffers are no longer attached to the global window root. System `__h` code
   is built as `libohos_video_overlay.so` and loaded over a C ABI by Chromium.
5. Demotion commits a no-hole primary and hides video together, then drains the
   old video queue. Re-promotion waits for drain completion. Paused video can
   change geometry without another SetBuffer or ending its original read.
   Swap completion waits for replaced RS reads/fences; browser-held decoder
   leases survive a GPU disconnect. Close is asynchronous with no fake release.

Native video import whitelists accept read-only SCANOUT on Vulkan and Graphite;
the Ozone factory also accepts the presenter's SDR RGBA/BGRA primary. Vulkan
overlay access duplicates existing SYNC_FD dependencies and merges multiple
fences without CPU waiting. A failed merge restores every pending dependency
before returning failure, so fallback cannot reuse an unfinished decoder read.
For fork GLES, the startup flag is forwarded to the GPU and enables importing
browser-owned NV12/P010 parcels through the existing Ozone EGL path when native
image import and native fences are supported. It does not permit YUV allocation
in the child. Without this opt-in, GLES retains its previous decoder path.

The first processed primary reports GPU startup success and the independent
video path's HDR capability, without requiring a PQ webpage framebuffer. HDR
video retains native P010/PQ and static metadata; webpage text remains SDR. A
lost HDR display, rejected frame or broken channel disables the presenter and
requests output recreation through the existing compositor path. Reliable
physical presentation timestamps are unavailable: presentation feedback is
explicitly Failure, not a fabricated VSYNC timestamp.

Activation: set `enableVideoOverlays: true` on the WebWindow's startup config and
use a freshly compiled engine HAR containing the new native library. Do not set
it on the old native binary and assume the test is active. The current flag is
intentionally off by default until the following tests pass. No 154 runtime has
been built with this integration yet.

Local checks passed:

- Session ASan/UBSan: invalid planes, prepare failure, backpressure, stale
  releases, producer/release fences, asynchronous close, 100 paused geometry
  reuses, demotion/re-promotion and 500 recycled allocation generations.
- Linux socket test: descriptor transfer/ownership/CLOEXEC, malformed and
  truncated messages, geometry/format checks and EOF. This is a host test,
  not proof of device GPU IPC or browser promotion.
- ARM64 NDK bridge compiled/linked with `--no-undefined`, including native
  ContentSlot host. Production exports are restricted to the C interface.
- Engine ArkTS checks and HAR packaging passed using the existing native
  runtime; this does **not** establish that new Chromium C++ code compiles.
- A temporary ContentSlot HAP passed two create/submit/close cycles on the phone,
  including recreation in the same process. The original parent identity matched,
  the cyan primary stayed inside the clipped viewport, both release callbacks
  arrived, and no test nodes remained after close. This found and fixed local
  library symbol lookup (RTLD_DEFAULT could not see the ArkUI API). Temporary
  shell sources were restored and the normal HAP rebuilt. See
  [host evidence](../scripts/tests/ohos-video-overlay/content-host-evidence.json).

Pending: full 155 kernel build; actual webpage promotion for GL/Vulkan/Graphite; scroll/resize/fullscreen,
paused controls, CSS effects triggering demotion, re-promotion, live SDR/HDR
changes, tab close/navigation/background and GPU process loss. The private RS
compatibility gate remains firmware-specific, and other UI contexts fail closed.
The old decoder harness evidence must not be cited as proof of these new stages.
