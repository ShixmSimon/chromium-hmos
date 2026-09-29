// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_GPU_CHILD_CHANNEL_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_GPU_CHILD_CHANNEL_H_

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/time/time.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"

// The GPU process as a HarmonyOS native child of its own.
//
// Renderers are native children started with OH_Ability_StartNativeChildProcess,
// which passes arguments and file descriptors and nothing else. The GPU
// process needs one more thing: the XComponent windows it draws into, and
// those live in the browser process. HarmonyOS will not look a window up by
// surface id from another process, but it will carry one through an IPC
// parcel (OH_NativeWindow_WriteToParcel), and only a child started with
// OH_Ability_CreateNativeChildProcess has an IPC channel to its parent. So the
// GPU process is started that way, and the channel carries, in order:
//
//   1. its command line and file descriptors, which the other kind of child
//      gets from the launch call itself;
//   2. every window the browser knows, then every change to one.
//
// In the GPU process the windows land in a mirror of the browser's registry,
// and the registry's GPU-side lookups read the mirror instead.

namespace ui {

// Left in the user data directory by a run whose GPU child would not start,
// so the next launch keeps the GPU in the browser process. That launch
// removes it.
inline constexpr char kOhosGpuChildFailedMarker[] = "GpuChildFailed";

// Browser process. Registers the launcher base uses for --type=gpu-process.
void InstallOhosGpuChildLauncher();

// Browser process. Sends the GPU process whatever changed in the registry
// since the last call. Does nothing unless a GPU child is connected.
void ForwardOhosSurfacesToGpuChild();

// GPU process. True once the channel has been opened, which happens before
// Chromium starts in that process.
bool IsOhosGpuChildProcess();

// GPU process. Sends Chromium's log to hilog again, at WARNING and above.
// Called once Chromium has initialised in that process: from then on its
// log reached nowhere -- not even ERROR -- while the probes, which run
// before, did. Idempotent.
void AttachOhosGpuChildLogging();

// GPU process. The mirror's answers to the registry's GPU-side lookups.
std::optional<OhosNativeSurface> GetOhosGpuChildSurface(
    gfx::AcceleratedWidget widget);
bool IsOhosGpuChildSurfaceExpected(gfx::AcceleratedWidget widget);
bool IsOhosGpuChildAnchored(gfx::AcceleratedWidget widget);
std::optional<OhosNativeSurface> WaitForOhosGpuChildSurface(
    gfx::AcceleratedWidget widget,
    base::TimeDelta timeout);
int32_t GetOhosGpuChildApplicationWindowId(gfx::AcceleratedWidget widget);

// GPU process entry points, called from the child library's exports.
//
// Returns the OHIPCRemoteStub the system hands to the parent.
void* CreateOhosGpuChildStub();
// Tries the system's EGL and GLES directly, step by step, and logs each
// result: whether this process can reach the GPU at all, before ANGLE and
// Chromium make the same calls with less to say about them. Diagnostic.
void ProbeOhosGpuChildEgl();
// The same for Vulkan, when Skia was asked to use it: loader, instance
// extensions, instance, physical device, swapchain support. Chromium falls
// back to GL without a word when any of these fails. Diagnostic.
void ProbeOhosGpuChildVulkan();
// GPU process. Whether the browser asked Skia to use Vulkan, as it said in
// the bootstrap. Chromium carries that to the GPU process in the encoded
// --gpu-preferences, which is not readable before Chromium starts.
bool OhosGpuChildWantsVulkan();
// Blocks until the parent has sent the command line and descriptors.
// `fds` pairs each descriptor's number in the child with the descriptor.
bool WaitForOhosGpuChildBootstrap(std::string* encoded_params,
                                  std::vector<std::pair<int, int>>* fds);

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_OHOS_OHOS_GPU_CHILD_CHANNEL_H_
