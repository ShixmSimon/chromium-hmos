# OHOS WebNN / NNRt bring-up

This is a default-off **single Add graph prototype**, not general model support.
Float32 only, two distinct input operands, one output, identical shapes, rank
1–4, at most 4 MiB per tensor. Constants, broadcasting, multi-op graphs, CPU/GPU
contexts and WebGPU tensor interop are not supported. Native computation uses
an enumerated `OH_NN_ACCELERATOR`; there is no application-side CPU fallback.

## Before the device returns

- `bash scripts/test-webnn-nnrt.sh`: host fault injection against the installed
  SDK declarations, with ASan/UBSan. It does not execute NNRt or emulate a device.
- `bash scripts/build-webnn-probe.sh`: cross-compile and link an ARM64 probe.
  Set `OHOS_NATIVE_SDK` to the native SDK root on other machines. The probe links
  libc++ statically and does not require installing or replacing Lumie.
- The Chromium integration must pass the native CI build. Building the probe
  alone does **not** validate the Chromium/Mojo C++ integration.

## Device validation (not performed yet)

1. Use a Lumie package containing the new native engine and the new shell flag.
   Enable experimental multiprocess on phones (Skia Vulkan on tablets), enable
   WebNN (NNRt) under `lumie://flags`, and fully restart. Graphite is optional.
   Safe start / configurations without a separate GPU suppress this experiment.
2. Confirm `chrome://version` has both `WebMachineLearningNeuralNetwork` and
   `OhosWebNN` in the **same** enabled-features list, and inspect `chrome://gpu`.
   An old HAR plus the new shell toggle will not work.
3. Serve this directory on the host, reverse-forward the port with HDC, and open
   the localhost URL on the device. `index.html` runs two context lifetimes,
   twenty dispatch/read cycles with changing inputs, and unsupported-operation
   checks. It exports `window.runWebnnTest()` and `window.webnnReport` for CDP.
4. Collect `webnn` trace plus hilog. Require `OhosWebNN selected accelerator`,
   `OhosWebNN compiled Add`, `OhosWebNNDeviceSelected`, `OhosWebNNCompile`,
   `OhosWebNNDispatch`, and `OhosWebNNExecutionCompleted`. Confirm the reported
   backend is `OHOS NNRt` in WebNN introspection where available.
5. If context or graph creation fails, record the exact NNRt API/error code.
   The independent `nnrt-probe` helps distinguish driver/API access problems
   from Chromium integration, but shell-process success does not prove browser
   child-process success. Hardware profiling remains the definitive check of
   driver execution placement; a device label alone is not profiling evidence.
6. Verify opt-out/restart, context loss and foreground/background recovery.

This prototype intentionally copies two inputs into device tensors and one
output back per dispatch, in addition to WebNN IPC/JS transfers. It makes no
zero-copy claim. All NNRt API calls, native ownership and synchronous dispatches
run on one dedicated worker thread shared by the contexts, not the Viz main
thread. Native failures invalidate the context; subsequent reads cannot return
an old successful result as if the failed dispatch had succeeded.

## Build verification (2026-10-19)

Native commit `0d778a088f15195435479d4a4f16d9d549ee21d0`:
[ARM64 native CI](https://github.com/miramira8295/chromium-hmos/actions/runs/39221206546)
and [adapter CI](https://github.com/miramira8295/chromium-hmos/actions/runs/39221206499)
both passed. Lumie flag commit: `96552ed` on `main`; 36 relevant Node tests and
the focused ArkTS check passed. Host NNRt tests pass ASan/UBSan, including
31 construction and 10 execution failure points. The standalone ARM64 probe
also cross-compiles and links successfully. Device execution is still pending.

The downloaded `build-0d778a08/engine.har` matches the release SHA256SUMS:
`1f2a1d8e642f9d688eecab05eced72f6accba6b55f36d52ece72d19e4b61f350`.
