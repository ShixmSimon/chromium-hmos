#!/bin/bash
# Runs gclient hooks and assembles the HarmonyOS SDK subset Chromium links
# against. The local DevEco SDK ships LLVM 15.0.4 runtimes while the adapter
# expects the LLVM 19 layout, so version aliases point at the 15.0.4 files.
set -euo pipefail
source /mnt/exchange/scripts/env.sh

readonly SDK_SOURCE=/mnt/ohos-native
readonly SDK_ROOT=/work/ohos_sdk
readonly SDK_LLVM_VERSION=15.0.4

log "Running gclient hooks"
cd "$CHROMIUM_ROOT"
gclient runhooks

log "Assembling HarmonyOS SDK subset from $SDK_SOURCE"
readonly LLVM_LIB="$SDK_ROOT/native/llvm/lib"
mkdir -p "$LLVM_LIB/clang/$SDK_LLVM_VERSION/lib"
rsync -a "$SDK_SOURCE/sysroot/" "$SDK_ROOT/native/sysroot/"
rsync -a "$SDK_SOURCE/llvm/lib/aarch64-linux-ohos/" "$LLVM_LIB/aarch64-linux-ohos/"
rsync -a "$SDK_SOURCE/llvm/lib/clang/$SDK_LLVM_VERSION/include/" \
  "$LLVM_LIB/clang/$SDK_LLVM_VERSION/include/"
rsync -a "$SDK_SOURCE/llvm/lib/clang/$SDK_LLVM_VERSION/lib/aarch64-linux-ohos/" \
  "$LLVM_LIB/clang/$SDK_LLVM_VERSION/lib/aarch64-linux-ohos/"
cp "$SDK_SOURCE/oh-uni-package.json" "$SDK_ROOT/native/"

ln -sfn "$SDK_LLVM_VERSION" "$LLVM_LIB/clang/19"
ln -sfn "$SDK_LLVM_VERSION" "$LLVM_LIB/clang/current"
ln -sfn aarch64-linux-ohos \
  "$LLVM_LIB/clang/$SDK_LLVM_VERSION/lib/aarch64-unknown-linux-ohos"
ln -sfn "$SDK_ROOT" "$CHROMIUM_SRC/ohos_sdk"

test -f "$CHROMIUM_SRC/ohos_sdk/native/llvm/lib/clang/19/lib/aarch64-unknown-linux-ohos/libclang_rt.builtins.a"
test -d "$CHROMIUM_SRC/ohos_sdk/native/sysroot/usr/lib/aarch64-linux-ohos"
log "Hooks and SDK ready"
