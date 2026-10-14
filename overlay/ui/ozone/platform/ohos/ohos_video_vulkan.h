// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_VIDEO_VULKAN_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_VIDEO_VULKAN_H_
#include <memory>
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/gpu_memory_buffer_handle.h"
namespace gpu {
class VulkanDeviceQueue;
class VulkanImage;
}  // namespace gpu
namespace ui {
bool CanImportOhosVideoVulkan(gpu::VulkanDeviceQueue* queue);
std::unique_ptr<gpu::VulkanImage> ImportOhosVideoVulkan(
    gpu::VulkanDeviceQueue* queue,
    gfx::GpuMemoryBufferHandle handle,
    const gfx::Size& size,
    const gfx::ColorSpace& color_space);
}  // namespace ui
#endif
