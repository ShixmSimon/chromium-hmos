// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/ohos/ohos_gpu_capability_probe.h"

#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include <string>
#include <vector>

#include "base/logging.h"

namespace ui {
namespace {

constexpr const char* kInterestingDeviceExtensions[] = {
    "VK_OHOS_external_memory",
    "VK_OHOS_native_buffer",
    "VK_KHR_external_memory",
    "VK_KHR_external_memory_fd",
    "VK_EXT_external_memory_dma_buf",
    "VK_EXT_queue_family_foreign",
    "VK_KHR_external_semaphore",
    "VK_KHR_external_semaphore_fd",
    "VK_KHR_external_fence_fd",
    "VK_KHR_sampler_ycbcr_conversion",
    "VK_KHR_dedicated_allocation",
    "VK_EXT_image_drm_format_modifier",
};

constexpr const char* kInterestingEglExtensions[] = {
    "EGL_ANDROID_native_fence_sync",
    "EGL_KHR_fence_sync",
    "EGL_KHR_wait_sync",
    "EGL_KHR_reusable_sync",
    "EGL_OHOS_image_native_buffer",
    "EGL_EXT_image_dma_buf_import",
};

bool Contains(const std::string& list, const char* name) {
  const std::string padded = " " + list + " ";
  return padded.find(" " + std::string(name) + " ") != std::string::npos;
}

void ProbeVulkan() {
  void* library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    LOG(WARNING) << "OHOS GPU probe: no libvulkan.so";
    return;
  }
  auto get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      dlsym(library, "vkGetInstanceProcAddr"));
  auto create_instance = get_instance_proc
                             ? reinterpret_cast<PFN_vkCreateInstance>(
                                   get_instance_proc(nullptr,
                                                     "vkCreateInstance"))
                             : nullptr;
  if (!create_instance) {
    LOG(WARNING) << "OHOS GPU probe: no vkCreateInstance";
    return;
  }

  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "chromium-ohos-probe";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo create = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  create.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  const VkResult created = create_instance(&create, nullptr, &instance);
  if (created != VK_SUCCESS) {
    LOG(WARNING) << "OHOS GPU probe: vkCreateInstance " << created;
    return;
  }

#define PROBE_PROC(type, name) \
  auto name = reinterpret_cast<type>(get_instance_proc(instance, #name))
  PROBE_PROC(PFN_vkDestroyInstance, vkDestroyInstance);
  PROBE_PROC(PFN_vkEnumeratePhysicalDevices, vkEnumeratePhysicalDevices);
  PROBE_PROC(PFN_vkGetPhysicalDeviceProperties, vkGetPhysicalDeviceProperties);
  PROBE_PROC(PFN_vkEnumerateDeviceExtensionProperties,
             vkEnumerateDeviceExtensionProperties);
  PROBE_PROC(PFN_vkGetPhysicalDeviceExternalSemaphoreProperties,
             vkGetPhysicalDeviceExternalSemaphoreProperties);
#undef PROBE_PROC

  uint32_t device_count = 0;
  if (vkEnumeratePhysicalDevices) {
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
  }
  std::vector<VkPhysicalDevice> devices(device_count);
  if (device_count) {
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
  }
  for (VkPhysicalDevice device : devices) {
    VkPhysicalDeviceProperties properties = {};
    vkGetPhysicalDeviceProperties(device, &properties);
    uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count,
                                         nullptr);
    std::vector<VkExtensionProperties> extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count,
                                         extensions.data());
    std::string all;
    for (const VkExtensionProperties& extension : extensions) {
      all += std::string(extension.extensionName) + " ";
    }
    std::string summary;
    for (const char* name : kInterestingDeviceExtensions) {
      summary += std::string(name) + (Contains(all, name) ? "=yes " : "=no ");
    }
    LOG(WARNING) << "OHOS GPU probe: Vulkan device " << properties.deviceName
                 << " api " << VK_API_VERSION_MAJOR(properties.apiVersion)
                 << "." << VK_API_VERSION_MINOR(properties.apiVersion) << "."
                 << VK_API_VERSION_PATCH(properties.apiVersion) << ", "
                 << extension_count << " extensions: " << summary;
    // Every extension name containing OHOS, in case there are others.
    std::string ohos;
    for (const VkExtensionProperties& extension : extensions) {
      if (std::string(extension.extensionName).find("OHOS") !=
          std::string::npos) {
        ohos += std::string(extension.extensionName) + " ";
      }
    }
    LOG(WARNING) << "OHOS GPU probe: Vulkan OHOS extensions: " << ohos;

    if (vkGetPhysicalDeviceExternalSemaphoreProperties) {
      VkPhysicalDeviceExternalSemaphoreInfo info = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
      info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      VkExternalSemaphoreProperties semaphore = {
          VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
      vkGetPhysicalDeviceExternalSemaphoreProperties(device, &info,
                                                     &semaphore);
      LOG(WARNING) << "OHOS GPU probe: sync-fd semaphore features 0x"
                   << std::hex << semaphore.externalSemaphoreFeatures
                   << " (1 exportable, 2 importable)";
    }
  }
  if (vkDestroyInstance) {
    vkDestroyInstance(instance, nullptr);
  }
}

void ProbeEgl() {
  // The wrapper ANGLE reaches the system EGL through
  // (OHOS_ANGLE_WRAPPER_EXPORTS); its default display is the one ANGLE
  // initialised, so it is only read here, never initialised.
  void* library = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    LOG(WARNING) << "OHOS GPU probe: no libEGL.so";
    return;
  }
  using GetDisplay = void* (*)(void*);
  using QueryString = const char* (*)(void*, int);
  auto get_display =
      reinterpret_cast<GetDisplay>(dlsym(library, "eglGetDisplay"));
  auto query_string =
      reinterpret_cast<QueryString>(dlsym(library, "eglQueryString"));
  if (!get_display || !query_string) {
    return;
  }
  constexpr int kEglExtensions = 0x3055;
  const char* extensions = query_string(get_display(nullptr), kEglExtensions);
  if (!extensions) {
    LOG(WARNING) << "OHOS GPU probe: EGL display not initialised here";
    return;
  }
  std::string summary;
  for (const char* name : kInterestingEglExtensions) {
    summary +=
        std::string(name) + (Contains(extensions, name) ? "=yes " : "=no ");
  }
  LOG(WARNING) << "OHOS GPU probe: EGL " << summary;
  LOG(WARNING) << "OHOS GPU probe: EGL all: " << extensions;
}

}  // namespace

void ProbeOhosGpuSharingCapabilities() {
  static bool probed = false;
  if (probed) {
    return;
  }
  probed = true;
  ProbeEgl();
  ProbeVulkan();
}

}  // namespace ui
