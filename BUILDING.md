# Build Notes

## Prerequisites

- A complete Chromium checkout with `depot_tools` and all DEPS synchronized.
- Linux or WSL for Chromium native compilation. On Apple silicon macOS, use an
  x86_64 container as described in
  [docs/BUILDING_MACOS_CONTAINER.md](docs/BUILDING_MACOS_CONTAINER.md).
- A compatible HarmonyOS SDK/NDK and Rust toolchain supplied by the developer.
- DevEco Studio/Hvigor for the ArkUI HAP shell.

## Isolated CI source trees

This branch pins Chromium **155.0.8059.91** at
`f043649a0ef1fd96e48d56ddbfc8a4a99aa2ca37`. The stable 154 baseline and its
working build directory stay intact.

| Adapter branch | Native source | Native output |
| --- | --- | --- |
| `hmos-154-adapter` | `/root/chromium-154/src` | `out/ohos_arm64` inside 154 |
| `hmos-155-adapter` | `/root/chromium-155/src` | `out/ohos_arm64` inside 155 |

Native compilation runs through push-triggered GitHub Actions. Push source
changes to the corresponding adapter branch; `select-ci-engine.py` rejects
other refs, and both branches share a serial build queue.

## Chromium 155 preparation

The 155 job runs `scripts/prepare-chromium-155.sh`. It seeds independent files
and Git objects from the 154 checkout without its native outputs, synchronizes
the new DEPS with shallow history, and uses a pinned bootstrap tool checkout
at `/root/chromium-155/depot_tools`. Private SDK links are restored after
upstream synchronization. The existing licensed SDK exposes its compiler
runtime through `native/llvm/lib/clang/current/`; the 155 job reuses those
links.

The job verifies every revision and payload hash in
`patches/migration-155/manifest.json`, applies the native and dependency
snapshots, synchronizes the overlay by content and runs toolchain hooks.
Unchanged compiler inputs retain their timestamps during retries.

`scripts/apply-adapter.sh` and `patches/deps/series` describe the historical
154 setup. The 155 workflow uses `patches/migration-155/` instead.

## Native build

CI generates `out/ohos_arm64` and validates V8 PGO/context snapshots, compiles
critical OHOS graphics, video, certificate and browser objects first, and
then builds `gn_all`. Compiler diagnostics are retained in `build-status/`.
Native rule validation and engine HAR packaging follow a successful build.

The build uses Chromium branding and includes the V8 JIT compilers
(Sparkplug, Maglev and TurboFan). At launch the runner probes whether the
process may map executable memory: where it may -- tablets and 2in1, and a
phone running a debug-signed package -- JIT is on; where it may not -- a
released phone package -- or when the startup configuration asks for it, V8
runs with `--jitless` and JavaScript is interpreted (about five times slower
on a simple loop on a phone). `aboutInfo.jitEnabled` reports the current
launch.

WebAssembly needs the DrumBrake interpreter when V8 is JITless, so the build
carries it (`v8_enable_drumbrake = true`, with its bounds checks): a JITless
launch still runs WebAssembly, interpreted, through `--wasm-jitless`.
It also enables the Chromium AAC/H.264 build switches used by the validated
test package. Those switches do not grant codec patent or distribution rights;
distributors remain responsible for the licenses required in their markets.

The 155 bootstrap copies `config/args.plan_kirin_pc.gn` only when the output
has no arguments file. Subsequent builds retain that tree's arguments; the
profile helper synchronizes the repository's V8 PGO and context snapshot
choices. The 154 workflow continues using its existing arguments file.

## HAP

The ArkUI project is under `chromium-ui`. Configure signing locally in DevEco
Studio. Do not commit generated signing blocks or `.cer`, `.p12`, or `.p7b`
files. The native build must be staged into the ArkUI project before running
`devecocli build --product default --build-mode release`.

This publication is a source snapshot, not a promise that every third-party
machine has the same private SDK build used for the original test package.
