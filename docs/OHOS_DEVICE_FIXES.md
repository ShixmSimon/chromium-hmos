# HarmonyOS build and device fixes

The published adapter patch covers files tracked by `chromium/src` only. A
fresh checkout of the pinned revision also needs changes in DEPS repositories
and a few runtime fixes found on a physical phone. `scripts/apply-adapter.sh`
applies all of them.

Verified on a HUAWEI Mate 70 Pro+ running OpenHarmony 7.0 (API 26): the
unsigned HAP installs with a debug signature, starts, renders pages through
GPU compositing, shows CJK text, and serves mobile layouts on sites that detect
HarmonyOS by the ArkWeb User-Agent token.

## DEPS repositories (`patches/deps/`)

`patches/deps/series` maps each patch to its repository relative to `src`.

| Patch | Why |
| --- | --- |
| `v8.patch` | `v8.gni` only allows DrumBrake on win/linux/mac/ios. `BUILD.gn` sets no `V8_HAVE_TARGET_OS` for `ohos`, so `v8config.h` defines an empty `V8_TARGET_OS_LINUX` and `#if V8_TARGET_OS_LINUX` fails; OHOS maps to Linux. OHOS also needs the POSIX/arm64 trap handler and Linux libbase sources. Jitless arm64 builtins leave `is_code` unused, and musl leaves `HAVE_EXECINFO_H` unset. |
| `third_party_angle.patch` | Mirrors Huawei ArkWeb 144's `is_ohos` conditions (Linux system utils, GPU info, Vulkan display, dma-buf, EGL backend). `DisplayEGL` loads `libEGL.so` on OHOS. |
| `third_party_dawn.patch` | `src/dawn/common:common` is hidden on unknown OSes; `SystemEvent.cpp` includes `<poll.h>` because musl warns on `<sys/poll.h>`. |
| `third_party_ffmpeg.patch` | No `chromium/config/Chrome/ohos` exists. OHOS uses the Linux config, as ChromeOS, Fuchsia, and Huawei ArkWeb 144 do. The Linux arm64 `HAVE_*` libc entries match ArkWeb 144's OHOS-generated config. |
| `third_party_pdfium.patch` | OHOS uses the Android font platform and `CFX_Face` OS/2 helpers (Huawei ArkWeb 144). |
| `third_party_perfetto.patch`, `third_party_webrtc.patch` | musl's `CMSG_NXTHDR` trips `-Wsign-compare` (same suppression as the adapter's `base` and `mojo` changes). WebRTC uses the Linux thread id/name paths without `<linux/prctl.h>`. |
| `third_party_skia.patch` | Adds `SkFontMgr_New_Custom_Directories`, which `skia/ext/font_utils.cc` calls, and implements `onMatchFamilyStyleCharacter`. Blink's OHOS font cache relies on it for all fallback; without it every CJK glyph was blank. The locale's `SC`/`TC`/`JP`/`KR` family is preferred. |
| `third_party_vulkan-loader_src.patch` | Defines `SYSCONFDIR` and `FALLBACK_*_DIRS` like Linux. The ICD search paths still need device verification. |

## `chromium/src` additions (`patches/chromium-150-harmonyos.patch`)

- `third_party/dav1d/BUILD.gn`: musl lacks `pthread_{get,set}affinity_np`.
- `ui/webui/resources/js/BUILD.gn`: builds `batch_upload_promo` on OHOS, which
  the adapter already allows and Settings imports.
- `components/pdf/renderer/pdf_view_web_plugin_client.h`: Screen AI include and
  OCR members follow `ENABLE_SCREEN_AI_SERVICE`, which the adapter disables.
- `chrome/browser/ui/webui/intro/intro_ui.cc`: completes the adapter's OHOS
  early return by excluding DICE-only intro resources.
- `chrome/browser/chrome_content_browser_client.cc`: phone UI uses Chrome for
  Android viewport behavior and reports touch input (`maxTouchPoints`,
  `ontouchstart`, `(pointer: coarse)`, `(hover: none)`); Ozone OHOS registers
  no touchscreen device.
- `components/embedder_support`: OHOS uses ArkWeb's default User-Agent format,
  for example
  `Mozilla/5.0 (Phone; OpenHarmony 7.0) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36 ArkWeb/7.0.0.105 Mobile`.
  On the test phone, Baidu served its desktop page for the previous
  `(HarmonyOS; arm64)` string and for OpenHarmony strings without the ArkWeb
  token.

## Overlay

`ohos_nweb/src/aura_shell/ohos_chrome_main_runner.cc`:

- `--use-angle=gles-egl` replaces `--use-angle=vulkan`. The phone's Vulkan
  driver lacks `VK_KHR_display`, which ANGLE's Linux Vulkan display requires,
  so `eglInitialize` failed and pages stayed blank. Neither upstream ANGLE nor
  Huawei ArkWeb 144's ANGLE has an OHOS Vulkan WSI.
- Phone UI enables `OverlayScrollbar` in the single `--enable-features` switch.

## Known issues

- `eglCreateSync` fails on the native EGL, which loses a WebGL context and
  recreates the GPU context on some pages.
- The build links the HarmonyOS SDK LLVM 15.0.4 runtimes through aliases for the
  LLVM 19 layout the adapter expects; see `BUILDING_MACOS_CONTAINER.md`.
