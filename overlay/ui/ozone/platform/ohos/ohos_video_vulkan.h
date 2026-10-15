// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_VIDEO_VULKAN_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_VIDEO_VULKAN_H_
#include <memory>
#include "base/memory/scoped_refptr.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/gpu_memory_buffer_handle.h"
#include "ui/gfx/native_pixmap.h"
namespace gpu {
class VulkanDeviceQueue;
class VulkanImage;
}  // namespace gpu
namespace ui {
bool CanImportOhosVideoVulkan(gpu::VulkanDeviceQueue* queue);
// Offscreen RGBA/BGRA images share the same native allocation with Dawn/GL.
// The VulkanImage retains its pixmap until deferred GPU cleanup completes.
std::unique_ptr<gpu::VulkanImage> ImportOhosOffscreenVulkan(
    gpu::VulkanDeviceQueue* queue,
    scoped_refptr<gfx::NativePixmap> pixmap);
std::unique_ptr<gpu::VulkanImage> ImportOhosOffscreenVulkan(
    gpu::VulkanDeviceQueue* queue,
    gfx::GpuMemoryBufferHandle handle,
    const gfx::Size& size);
// Same-process decoder path. The VkImage owns the Surface queue lease until
// fence-helper cleanup; no parcel, socket, or CPU pixel mapping is needed.
std::unique_ptr<gpu::VulkanImage> ImportOhosVideoVulkan(
    gpu::VulkanDeviceQueue* queue,
    scoped_refptr<gfx::NativePixmap> pixmap,
    const gfx::ColorSpace& color_space);
std::unique_ptr<gpu::VulkanImage> ImportOhosVideoVulkan(
    gpu::VulkanDeviceQueue* queue,
    gfx::GpuMemoryBufferHandle handle,
    const gfx::Size& size,
    const gfx::ColorSpace& color_space);
}  // namespace ui
#endif
