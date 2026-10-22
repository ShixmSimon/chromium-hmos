// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GPU_VULKAN_OHOS_VULKAN_IMPLEMENTATION_OHOS_H_
#define GPU_VULKAN_OHOS_VULKAN_IMPLEMENTATION_OHOS_H_

#include <memory>
#include <vector>

#include "base/component_export.h"
#include "base/functional/callback.h"
#include "gpu/vulkan/vulkan_implementation.h"
#include "gpu/vulkan/vulkan_instance.h"
#include "ui/gfx/vsync_provider.h"

namespace gpu {

class COMPONENT_EXPORT(VULKAN_OHOS) VulkanImplementationOhos
    : public VulkanImplementation {
 public:
  // The OHNativeWindow* for a widget, or null. The windows are the
  // platform's business -- they live in ozone's registry, which in a GPU
  // process of its own is a mirror of the browser's -- so the platform
  // that creates this says how to find them.
  using WindowResolver =
      base::RepeatingCallback<void*(gfx::AcceleratedWidget)>;

  // The display timing for a widget's surface: HarmonyOS's NativeVSync,
  // which lives in ozone like the windows do. Null, or a factory that
  // returns null, leaves VulkanSurface's fixed 60 Hz guess.
  using VSyncProviderFactory =
      base::RepeatingCallback<std::unique_ptr<gfx::VSyncProvider>(
          gfx::AcceleratedWidget)>;

  VulkanImplementationOhos(bool force_native,
                           WindowResolver resolve_window,
                           VSyncProviderFactory create_vsync_provider = {});

  VulkanImplementationOhos(const VulkanImplementationOhos&) = delete;
  VulkanImplementationOhos& operator=(const VulkanImplementationOhos&) = delete;

  ~VulkanImplementationOhos() override;

  // VulkanImplementation:
  bool InitializeVulkanInstance(bool using_surface) override;
  VulkanInstance* GetVulkanInstance() override;
  std::unique_ptr<VulkanSurface> CreateViewSurface(
      gfx::AcceleratedWidget window) override;
  bool GetPhysicalDevicePresentationSupport(
      VkPhysicalDevice device,
      const std::vector<VkQueueFamilyProperties>& queue_family_properties,
      uint32_t queue_family_index) override;
  std::vector<const char*> GetRequiredDeviceExtensions() override;
  std::vector<const char*> GetOptionalDeviceExtensions() override;
  VkFence CreateVkFenceForGpuFence(VkDevice vk_device) override;
  std::unique_ptr<gfx::GpuFence> ExportVkFenceToGpuFence(
      VkDevice vk_device,
      VkFence vk_fence) override;
  VkExternalSemaphoreHandleTypeFlagBits GetExternalSemaphoreHandleType()
      override;
  bool CanImportGpuMemoryBuffer(
      VulkanDeviceQueue* device_queue,
      gfx::GpuMemoryBufferType memory_buffer_type) override;
  std::unique_ptr<VulkanImage> CreateImageFromGpuMemoryHandle(
      VulkanDeviceQueue* device_queue,
      gfx::GpuMemoryBufferHandle gmb_handle,
      gfx::Size size,
      VkFormat vk_format,
      const gfx::ColorSpace& color_space) override;

 private:
  VulkanInstance vulkan_instance_;
  const WindowResolver resolve_window_;
  const VSyncProviderFactory create_vsync_provider_;
};

}  // namespace gpu

#endif  // GPU_VULKAN_OHOS_VULKAN_IMPLEMENTATION_OHOS_H_
