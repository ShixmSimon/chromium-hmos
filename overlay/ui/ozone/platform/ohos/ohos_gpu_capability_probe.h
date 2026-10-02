// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_GPU_CAPABILITY_PROBE_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_GPU_CAPABILITY_PROBE_H_

namespace ui {

// Diagnostic, once per process: logs whether the device's Vulkan driver and
// system EGL have what sharing OH_NativeBuffers between Dawn (Vulkan) and the
// GLES compositor would need -- VK_OHOS_external_memory and
// VK_OHOS_native_buffer, sync-fd semaphores, and an EGL native fence -- as
// "OHOS GPU probe" lines. Reads the EGL display ANGLE already initialised and
// never initialises one of its own. Call on the GPU thread after GL is up.
void ProbeOhosGpuSharingCapabilities();

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_OHOS_OHOS_GPU_CAPABILITY_PROBE_H_
