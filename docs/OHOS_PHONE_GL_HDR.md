# Phone fork OpenGL HDR output (154 / 155)

Phone fork GLES now negotiates color independently of Vulkan. The shell may
request `vulkanOutputMode: 'auto'` with `skiaBackend: 'gl'`; the historical field
and `--ohos-vulkan-output` switch remain for compatibility. A panel HDR flag
alone still does not enable HDR.

Before Viz copies output capabilities, an opaque phone GL surface requests a
real HDR10 NativeBuffer, imports it through EGLImage, attaches it to an FBO and
checks completeness and 10/10/10/2 component precision. Native buffer allocation
and color-tag readback use the same browser broker as Vulkan. Only a successful
probe may publish HDR. Transparent surfaces keep SDR8. This implementation does
not separately advertise P3 output.

SDR content uses sRGB, HDR uses BT.2020/PQ. The Skia wrapper, native format,
per-frame color metadata and negotiated color generation must agree. SDR/HDR
transitions retire imports before cleaning/reconfiguring the queue. HDR
luminance updates retain the same format and use metadata from Viz's trusted
system brightness snapshot.

Temporary queue backpressure or a hidden surface draws to a same-precision
scratch FBO and drops the frame. A ten-bit frame never takes the eight-bit CPU
readback path. Import, metadata, fence or presentation failures revoke HDR and
restart the GPU with the existing session SDR8 marker. An unexpected GPU exit
while GL color negotiation was active also prevents repeated HDR retries.

## Validation

- `scripts/test-ohos-phone-gpu.sh` on Linux covers protocol schema, descriptor
  ownership, color generations, malformed metadata, SDR/HDR buffer retirement,
  equivalent platform color tags and exact GL component precision.
- `python3 scripts/tests/test-ohos-gpu-state-recovery.py` checks recovery policy.
- Lumie's `scripts/tests/process-mode.test.mjs` verifies phone GL requests auto
  output without changing the GLES backend or non-fork/tablet routing.
- Native compilation uses the existing Incremental build on each adapter branch.
  155 carries the Viz changes in its pinned snapshot; 154 applies
  `patches/ohos-phone-gl-hdr.patch` after the existing EGL/shared-output patches.

Real-device acceptance must additionally verify `OHOS phone GL: output verified
hdr=1`, an actually presented `mode=2 native_format=34` frame, webpage
`matchMedia('(dynamic-range: high)').matches`, and correct SDR/HDR appearance.
Exercise video enter/exit fullscreen, rotation, background/resume, transparent
popups, hidden DevTools, GPU recovery and return to SDR. Capability detection
and compilation are not physical HDR brightness measurements.
