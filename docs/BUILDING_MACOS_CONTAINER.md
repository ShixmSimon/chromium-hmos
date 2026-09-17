# Building on Apple silicon macOS

The OHOS toolchain asserts `host_os == "linux"` and `host_cpu == "x64"`, so a
Mac cannot build natively. This route runs an x86_64 Ubuntu container through
Apple `container` and Rosetta, then packages the HAP on macOS with `devecocli`.
It was used on an M4 Pro (14 cores, 64 GB) with an external APFS SSD.

Scripts live in `tools/macos-container/`: `host/` runs on macOS and
`container/` runs inside the build container.

## Storage layout

Apple `container` launches every service and container through a launchd
plist inside its app root. launchd refuses plists on volumes with ownership
disabled, which external drives usually are, so the app root must stay on the
internal disk.

Do not move `snapshots` or `containers` to another volume. A container rootfs
is cloned from its image snapshot; across volumes the sparse 513 GB ext4 image
is copied instead and filled 128 GiB of the internal disk in under a minute.
Only symlink `content`, `volumes`, and `builder` to the external drive:

```bash
ROOT="$HOME/Library/Application Support/com.apple.container"
DATA=/path/to/external/container-data
mkdir -p "$DATA"/{content,volumes,builder} "$ROOT"
for d in content volumes builder; do ln -s "$DATA/$d" "$ROOT/$d"; done
container system start --enable-kernel-install
```

The internal footprint stays around 10 GB (kernel, Ubuntu snapshot, build
dependencies). Chromium source and output live in a volume on the external
drive (about 40 GB used).

Grant Full Disk Access to `/usr/local/bin/container-apiserver` and the
`container-core-images` and `container-runtime-linux` plugins first.

## Disk watchdog

Start `host/disk-watchdog.sh` before downloading anything. It checks every five
seconds and stops the build, containers, and container services when the
internal disk, the external disk, or the build volume falls below its limit, or
when the internal disk loses 10 GiB within one interval.

```bash
export HMOS_BUILD_BASE=/path/to/external/chromium-hmos-build
export HMOS_EXTERNAL_MOUNT=/Volumes/External
tools/macos-container/host/disk-watchdog.sh
```

## Build container

```bash
container volume create -s 500G chromium-hmos
container run -d --name hmos-build --platform linux/amd64 --rosetta \
  --cpus 12 --memory 40G -v chromium-hmos:/work \
  --mount type=bind,source=/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native,target=/mnt/ohos-native,readonly \
  --mount type=bind,source="$HMOS_BUILD_BASE/exchange",target=/mnt/exchange \
  docker.io/library/ubuntu:24.04 sleep infinity
```

Copy `tools/macos-container/container/` to `$HMOS_BUILD_BASE/exchange/scripts`,
clone depot_tools and this repository into `/work/tools`, then run
`host/run-pipeline.sh`. It runs, in order and resuming after failures:

1. `10-fetch-chromium.sh`: shallow fetch of the pinned revision and DEPS.
2. `20-apply-adapter.sh`: `scripts/apply-adapter.sh`, including DEPS patches.
3. `30-install-deps.sh`: Chromium Linux build dependencies (as root).
4. `40-runhooks-and-sdk.sh`: hooks plus the HarmonyOS SDK subset.
5. `50-build-native.sh`: `ohos_build/build.sh -t n` and export of
   `chromium-ui`.

Ubuntu 24.04's git 2.43 is too old for current depot_tools; install git from
`ppa:git-core/ppa` first.

## HarmonyOS SDK subset

The adapter links against `lib/clang/19/lib/aarch64-unknown-linux-ohos`, but the
public DevEco native SDK ships LLVM 15.0.4. `40-runhooks-and-sdk.sh` copies the
sysroot and target runtimes, then adds `clang/19`, `clang/current`, and
`aarch64-unknown-linux-ohos` aliases to the 15.0.4 files. Linking and a
device run succeed with these aliases; a matching LLVM 19 SDK is still
preferable.

## Package and deploy

`chromium-ui` has no signing configuration and produces an unsigned HAP:

```bash
cd "$HMOS_BUILD_BASE/exchange/chromium-ui"
devecocli build --product default --build-mode release
```

For a device test, change `AppScope/app.json5` to your own bundle name, run
`devecocli auth login` and `devecocli signature generate --product default`,
then `host/deploy-and-capture.sh <label>` with `HMOS_DEVICE` and `HMOS_BUNDLE`
set. It installs the HAP, launches it once the phone is unlocked, and saves a
screenshot and the app's hilog. Keep signing output out of this repository.
