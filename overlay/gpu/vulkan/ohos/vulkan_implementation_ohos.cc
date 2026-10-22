// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/vulkan/ohos/vulkan_implementation_ohos.h"

#include <cstring>
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/notimplemented.h"
#include "gpu/vulkan/vulkan_device_queue.h"
#include "gpu/vulkan/vulkan_function_pointers.h"
#include "gpu/vulkan/vulkan_image.h"
#include "gpu/vulkan/vulkan_instance.h"
#include "gpu/vulkan/vulkan_surface.h"
#include "gpu/vulkan/vulkan_util.h"
#include "ui/gfx/gpu_fence.h"
#include "ui/gfx/gpu_memory_buffer_handle.h"

namespace gpu {

VulkanImplementationOhos::VulkanImplementationOhos(
    bool force_native,
    WindowResolver resolve_window,
    VSyncProviderFactory create_vsync_provider)
    : VulkanImplementation(false, false, force_native),
      vulkan_instance_(force_native),
      resolve_window_(std::move(resolve_window)),
      create_vsync_provider_(std::move(create_vsync_provider)) {}

VulkanImplementationOhos::~VulkanImplementationOhos() = default;

bool VulkanImplementationOhos::InitializeVulkanInstance(bool using_surface) {
  DCHECK(using_surface);
  std::vector<const char*> required_extensions = {
      VK_KHR_SURFACE_EXTENSION_NAME, "VK_OHOS_surface"};
  // Said at ERROR either way: when Vulkan was asked for and this fails,
  // Chromium falls back to GL quietly, and the reason is otherwise lost.
  if (!vulkan_instance_.BindUnassignedFunctionPointers(
          base::FilePath("libvulkan.so")))
    return false;
  uint32_t count = 0;
  bool extended_colors = false;
  if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) ==
      VK_SUCCESS) {
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateInstanceExtensionProperties(
            nullptr, &count, extensions.data()) == VK_SUCCESS) {
      for (const auto& extension : extensions)
        extended_colors |=
            std::strcmp(extension.extensionName,
                        VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0;
    }
  }
  if (extended_colors)
    required_extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
  LOG(WARNING) << "OHOS Vulkan: creating instance; requested_layers=0 "
               << "swapchain_colorspace=" << extended_colors;
  // Initialize's third argument is required LAYERS, not optional extensions.
  // Enable colorspace only after enumeration, and request no validation layers.
  const bool initialized =
      vulkan_instance_.InitializeInstance(required_extensions, {});
  LOG(ERROR) << "OHOS Vulkan: instance "
               << (initialized ? "initialized" : "failed to initialize");
  return initialized;
}

VulkanInstance* VulkanImplementationOhos::GetVulkanInstance() {
  return &vulkan_instance_;
}

namespace {

// From <vulkan/vulkan_ohos.h>, repeated rather than included: the headers
// this is built against are Khronos's and have never heard of this
// platform, while the loader on the device has the symbol.
constexpr VkStructureType kStructureTypeSurfaceCreateInfoOHOS =
    static_cast<VkStructureType>(1000453000);

struct VkSurfaceCreateInfoOHOS {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  void* window;  // OHNativeWindow*
};

using PFN_vkCreateSurfaceOHOS = VkResult(VKAPI_PTR*)(
    VkInstance instance,
    const VkSurfaceCreateInfoOHOS* create_info,
    const VkAllocationCallbacks* allocator,
    VkSurfaceKHR* surface);

}  // namespace

std::unique_ptr<VulkanSurface> VulkanImplementationOhos::CreateViewSurface(
    gfx::AcceleratedWidget window) {
  const VkInstance instance = vulkan_instance_.vk_instance();
  static const auto create_surface = reinterpret_cast<PFN_vkCreateSurfaceOHOS>(
      vkGetInstanceProcAddr(instance, "vkCreateSurfaceOHOS"));
  if (!create_surface) {
    LOG(ERROR) << "OHOS Vulkan: vkCreateSurfaceOHOS is missing from the "
                  "Vulkan loader";
    return nullptr;
  }

  // The widget is Chromium's number for the window, not the window: the
  // OHNativeWindow* is looked up. Passing the number itself, as the first
  // version did, handed the driver an address that means nothing.
  void* const native_window =
      resolve_window_ ? resolve_window_.Run(window) : nullptr;
  if (!native_window) {
    LOG(ERROR) << "OHOS Vulkan: no native window for widget " << window;
    return nullptr;
  }

  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSurfaceCreateInfoOHOS surface_create_info = {};
  surface_create_info.sType = kStructureTypeSurfaceCreateInfoOHOS;
  surface_create_info.window = native_window;
  const VkResult result =
      create_surface(instance, &surface_create_info, nullptr, &surface);
  if (result != VK_SUCCESS) {
    LOG(ERROR) << "OHOS Vulkan: vkCreateSurfaceOHOS() failed: " << result;
    return nullptr;
  }
  LOG(ERROR) << "OHOS Vulkan: surface created for widget " << window;

  // The panel's own timing, as the GL surface has: without it the
  // compositor ran on a fixed 60 Hz guess on a 120 Hz panel, and a content
  // frame rate it chose never reached the system.
  return std::make_unique<VulkanSurface>(
      instance, window, surface, /*acquire_next_image_timeout_ns=*/UINT64_MAX,
      create_vsync_provider_ ? create_vsync_provider_.Run(window) : nullptr);
}

bool VulkanImplementationOhos::GetPhysicalDevicePresentationSupport(
    VkPhysicalDevice device,
    const std::vector<VkQueueFamilyProperties>& queue_family_properties,
    uint32_t queue_family_index) {
  // As on Android: a device that can render can present, and there is no
  // platform query that would say otherwise.
  return true;
}

std::vector<const char*>
VulkanImplementationOhos::GetRequiredDeviceExtensions() {
  return {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
}

std::vector<const char*>
VulkanImplementationOhos::GetOptionalDeviceExtensions() {
  return {
      VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
      VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
      VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
      VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
      VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
      VK_EXT_HDR_METADATA_EXTENSION_NAME,
  };
}

VkFence VulkanImplementationOhos::CreateVkFenceForGpuFence(VkDevice vk_device) {
  NOTIMPLEMENTED();
  return VK_NULL_HANDLE;
}

std::unique_ptr<gfx::GpuFence> VulkanImplementationOhos::ExportVkFenceToGpuFence(
    VkDevice vk_device,
    VkFence vk_fence) {
  NOTIMPLEMENTED();
  return nullptr;
}

VkExternalSemaphoreHandleTypeFlagBits
VulkanImplementationOhos::GetExternalSemaphoreHandleType() {
  return VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
}

bool VulkanImplementationOhos::CanImportGpuMemoryBuffer(
    VulkanDeviceQueue* device_queue,
    gfx::GpuMemoryBufferType memory_buffer_type) {
  // Not yet. HarmonyOS shares buffers through VK_OHOS_native_buffer, the
  // counterpart of Android's AHardwareBuffer path, and importing one is a
  // piece of work of its own. Saying no here keeps the callers on a path
  // that works rather than one that half does.
  return false;
}

std::unique_ptr<VulkanImage>
VulkanImplementationOhos::CreateImageFromGpuMemoryHandle(
    VulkanDeviceQueue* device_queue,
    gfx::GpuMemoryBufferHandle gmb_handle,
    gfx::Size size,
    VkFormat vk_format,
    const gfx::ColorSpace& color_space) {
  NOTIMPLEMENTED();
  return nullptr;
}

}  // namespace gpu
