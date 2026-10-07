# Build Notes

## Prerequisites

- A complete Chromium checkout with `depot_tools` and all DEPS synchronized.
- Linux or WSL for Chromium native compilation. On Apple silicon macOS, use an
  x86_64 container as described in
  [docs/BUILDING_MACOS_CONTAINER.md](docs/BUILDING_MACOS_CONTAINER.md).
- A compatible HarmonyOS SDK/NDK and Rust toolchain supplied by the developer.
- DevEco Studio/Hvigor for the ArkUI HAP shell.

The SDK and signing files are intentionally not redistributed by this project.
Place or link the unpacked SDK at `src/ohos_sdk` after applying the adapter.
The adapter also expects the WebView interface tree at
`../deps_code/webview`; `scripts/apply-adapter.sh` installs the published copy.
It also applies `patches/deps/` to the DEPS checkouts, which must be clean.

The adapter links against the SDK's LLVM 19 runtime layout
(`native/llvm/lib/clang/19/lib/aarch64-unknown-linux-ohos`). The public DevEco
native SDK ships LLVM 15.0.4; aliasing the 15.0.4 runtimes to that layout links
and runs on OpenHarmony 7.0, as described in the macOS container guide.

## Prepare Chromium

```bash
ADAPTER_ROOT=/path/to/chromium-hmos-adapter
git -C /path/to/chromium/src checkout f405107495a07cb1bfcf687d4af8d91117098db6
gclient sync -D
"${ADAPTER_ROOT}/scripts/apply-adapter.sh" /path/to/chromium/src
mkdir -p /path/to/chromium/src/out/plan_kirin_pc
cp "${ADAPTER_ROOT}/config/args.plan_kirin_pc.gn" \
  /path/to/chromium/src/out/plan_kirin_pc/args.gn
```

## Native Targets

From the Chromium source directory:

```bash
gn gen out/plan_kirin_pc
autoninja -C out/plan_kirin_pc \
  chrome libweb_engine web_render libnweb_render
```

The build uses Chromium branding and includes the V8 JIT compilers
(Sparkplug, Maglev and TurboFan). At launch the runner probes whether the
process may map executable memory: where it may -- tablets and 2in1, and a
phone running a debug-signed package -- JIT is on; where it may not -- a
released phone package -- or when the startup configuration asks for it, V8
runs with `--jitless` and JavaScript is interpreted (about five times slower
on a simple loop on a phone). `aboutInfo.jitEnabled` reports the current
launch.

WebAssembly needs the DrumBrake interpreter when V8 is JITless, and the
build has it off (`v8_enable_drumbrake = false`), so a JITless launch has no
`WebAssembly` object at all. Turning it on changes the runner's `args.gn` and
rebuilds V8 and what depends on it.
It also enables the Chromium AAC/H.264 build switches used by the validated
test package. Those switches do not grant codec patent or distribution rights;
distributors remain responsible for the licenses required in their markets.

Incremental CI reuses the runner's `${CHROMIUM_SRC}/${OUT_DIR}/args.gn`
(`OUT_DIR` defaults to `out/ohos_arm64`); it does not copy this repository's
`config/args.plan_kirin_pc.gn`. That file is kept identical to the runner's
`args.gn`; change both together, or the repository describes a build that CI
does not make.

## HAP

The ArkUI project is under `chromium-ui`. Configure signing locally in DevEco
Studio. Do not commit generated signing blocks or `.cer`, `.p12`, or `.p7b`
files. The native build must be staged into the ArkUI project before running
`devecocli build --product default --build-mode release`.

This publication is a source snapshot, not a promise that every third-party
machine has the same private SDK build used for the original test package.
