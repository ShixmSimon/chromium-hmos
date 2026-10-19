# Startup ready/ack and resumed-frame handoff

Implementation: native `08a2d3bb` and Lumie `main` commit `80ea4d8`.
This addresses the generic 4-second page-text timeout and premature startup-mask
release found in `OHOS_STARTUP_PROBE_VALIDATION.md`. EGL/GameAwareLayer behavior
is unchanged.

## Protocol and lifetime

- `startupProbe {startupToken, startupPhase, requestId}` / `startupAck` use the
  existing sequenced per-window command channel and targeted runtime events.
- `ready` reads only the dedicated offline document attribute in the renderer's
  isolated world. A native/browser-only acknowledgment cannot confirm startup.
- The shell waits at most 600 ms for an individual reply and retries after
  200 ms. Request ID, phase and startup token must all match. Native deduplicates
  in-flight work; a 4-second native watchdog permits replacing a stalled callback
  without making each shell retry wait four seconds.
- Renderer readiness enables navigation/session restoration, but leaves a separate
  `modeStartupMask` up. Chromium session restoration reports completion only after
  final active-tab selection (or saved-address fallback).
- `frame` is requested after those commands. It rejects the probe document,
  pending navigation, hidden content and inactive/destroyed documents. A
  document-scoped weak pointer and observer invalidation cover tab switches and
  primary-frame navigation. Window targeting never falls back to a global window.
- A successful `InsertVisualStateCallback` proves that the current document has
  submitted its visual state to Viz. The window then requests a successful
  presentation callback and schedules a redraw, including for static, blank and
  error pages. The shell confirms the mode and removes the mask only after this
  second acknowledgment in the foreground.
- This evidence means **document submission followed by successful window output
  feedback**. It does not claim hardware scanout of an exact document frame:
  OHOS output paths may synthesize successful presentation feedback after buffer
  submission. The new probes distinguish document submission, window feedback and
  shell mask-release request; ArkUI's actual removal frame is not timestamped.
- Each phase has a 30-second foreground tick budget independent of per-request
  waits. Background callbacks cannot confirm or release the mask. Failure retains
  the recovery overlay and unconfirmed-mode rollback policy. Stop/dispose cancels
  timers and native work; old async generations cannot resume the new lifecycle.

## Validation

Shell tests exercise lost commands, stale request/token/phase replies, background
ready/presentation, async restoration and fallback, stop/dispose, re-entry, and
foreground timeouts. Existing process-mode persistence/rollback tests remain in
place. Device measurements and the native build result follow.

### Build and installed binary (2026-10-19)

- Native CI: https://github.com/miramira8295/chromium-hmos/actions/runs/39243326403
  succeeded; native compilation/link completed in 150 s (3 steps).
- Engine commit: `08a2d3bb0efe17c1f5d03101b38eb2bdacb6e709`.
- ELF Build ID: `455ea0c7a47ce1ad91c0b7be59002bc2b03a906e`.
- Native SHA256: `2306dd4103e01c100f316f48913d4f152b4cfee3eac87046a8b65ffa6f012e44`.
  The signed phone HAP's native library matches the downloaded CI HAR exactly.
- Lumie: 42 tests pass; `devecocli build --modules entry` succeeded; installation
  preserved data and `devecocli run --skip-build` reported `Smoke: PASS`.
- Standalone ArkTS checking, after the HAR update, reported only the existing
  generated `BuildProfile` import in `LibraryPages.ets`; the full build resolves
  it and passed. No new protocol type errors remained.

### Mate 70 Pro+ process cold starts

Three force-stop/start samples, with a 42-second observation window each so the
existing 30-second healthy-start marker can settle. Times below are milliseconds
relative to the host issuing `aa start`, not launcher touch-to-first-frame. Only
this phone was measured; no Pad testing in this change.

| Run | Shell health start | Renderer ready ack | Resumed document submitted | Window success feedback | Mask release request |
|---|---:|---:|---:|---:|---:|
| 1 | 520.0 | 1589.0 | 2579.6 | 2827.1 | 2829.0 |
| 2 | 447.0 | 2152.6 | 2616.0 | 2771.5 | 2774.0 |
| 3 | 442.0 | 2178.0 | 2763.7 | 2788.2 | 2790.0 |

All three runs:

- Document submission preceded window success feedback, which preceded the shell
  mask release request by 1.8–2.5 ms. From shell health start to mask release was
  2309 / 2327 / 2348 ms.
- Actual backend `GraphiteDawnVulkan`; TreesInViz enabled; GPU crash count 0.
- Six tabs restored, no remaining startup probe page. Screenshots show the
  restored scroll page and its prior position, with no residual startup overlay.
- No generic `getPageText` startup requests. Each run exercised one 600-ms startup
  request timeout and recovered by retry. The post-install launch also exercised
  this path and released the mask after 2862 ms from health start.
- These samples verify the new bounded retry path and handoff ordering; they are
  not a statistical guarantee that all startup long tails have been eliminated.
  The previous mask came down before GPU output, so its timing is not an
  equivalent measure of visible startup completion.
- EGL still took 1464–1557 ms, with 1222–1270 ms beyond its thread CPU time. The
  separately identified EGL/GameAwareLayer wait remains outside this change.

Background/stop/re-entry, restoration exceptions and timeout recovery were
verified in deterministic shell tests, not injected into these device runs.
Blank/error-page redraw handling is implemented but was not separately exercised
on the device. No physical scanout timing or frame-by-frame flash assertion is
made from the screenshots.

Artifacts: `artifacts/startup-handoff/device-08a2d3bb/` contains `summary.json`,
per-run GPU reports, `chrome-gpu.txt`, screenshots, stage logs and HAP verification.
`artifacts/startup-handoff/build-08a2d3bb/verification.json` records the HAR and
ELF identity. Log collection has stopped, the CDP port forward has been removed,
and the original scroll tab is back in front. Brightness/volume were unchanged.
