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

The tested configuration uses Chromium branding, keeps V8 JavaScript enabled
in JITless mode, and executes WebAssembly through the DrumBrake interpreter.
It also enables the Chromium AAC/H.264 build switches used by the validated
test package. Those switches do not grant codec patent or distribution rights;
distributors remain responsible for the licenses required in their markets.

## HAP

The ArkUI project is under `chromium-ui`. Configure signing locally in DevEco
Studio. Do not commit generated signing blocks or `.cer`, `.p12`, or `.p7b`
files. The native build must be staged into the ArkUI project before running
`devecocli build --product default --build-mode release`.

This publication is a source snapshot, not a promise that every third-party
machine has the same private SDK build used for the original test package.
