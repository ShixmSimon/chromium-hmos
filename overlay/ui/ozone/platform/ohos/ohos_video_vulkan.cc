// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "ui/ozone/platform/ohos/ohos_video_vulkan.h"
#include <native_buffer/native_buffer.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_ohos.h>
#include "base/logging.h"
#include "gpu/vulkan/vulkan_device_queue.h"
#include "gpu/vulkan/vulkan_function_pointers.h"
#include "gpu/vulkan/vulkan_image.h"
#include "gpu/vulkan/vulkan_ycbcr_info.h"
#include "ui/ozone/platform/ohos/ohos_native_pixmap.h"

namespace ui {
namespace {
using GetProperties = PFN_vkGetNativeBufferPropertiesOHOS;
GetProperties GetQuery(VkDevice device) {
  return reinterpret_cast<GetProperties>(
      vkGetDeviceProcAddr(device, "vkGetNativeBufferPropertiesOHOS"));
}
}  // namespace

bool CanImportOhosVideoVulkan(gpu::VulkanDeviceQueue* queue) {
  if (!queue ||
      !queue->enabled_extensions().contains(
          VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME) ||
      !queue->enabled_extensions().contains(
          VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) ||
      !queue->enabled_extensions().contains(
          VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME) ||
      !GetQuery(queue->GetVulkanDevice()))
    return false;
  // NativeBuffer-specific properties are checked again for every import.
  return true;
}

std::unique_ptr<gpu::VulkanImage> ImportOhosOffscreenVulkan(
    gpu::VulkanDeviceQueue* queue,
    gfx::GpuMemoryBufferHandle handle,
    const gfx::Size& size) {
  if (!CanImportOhosVideoVulkan(queue) || handle.type != gfx::NATIVE_PIXMAP)
    return nullptr;
  const auto& prefix = handle.native_pixmap_handle().ohos_buffer_prefix;
  if (prefix.size() != 14)
    return nullptr;
  viz::SharedImageFormat format;
  if (prefix[8] == NATIVEBUFFER_PIXEL_FMT_RGBA_8888)
    format = viz::SinglePlaneFormat::kRGBA_8888;
  else if (prefix[8] == NATIVEBUFFER_PIXEL_FMT_BGRA_8888)
    format = viz::SinglePlaneFormat::kBGRA_8888;
  else
    return nullptr;
  return ImportOhosOffscreenVulkan(
      queue, ImportOhosOffscreenPixmap(
                 size, format, std::move(handle).native_pixmap_handle()));
}

std::unique_ptr<gpu::VulkanImage> ImportOhosOffscreenVulkan(
    gpu::VulkanDeviceQueue* queue,
    scoped_refptr<gfx::NativePixmap> pixmap) {
  if (!CanImportOhosVideoVulkan(queue) || !pixmap)
    return nullptr;
  const auto format = pixmap->GetSharedImageFormat();
  const bool bgra = format == viz::SinglePlaneFormat::kBGRA_8888;
  if (!bgra && format != viz::SinglePlaneFormat::kRGBA_8888)
    return nullptr;
  const auto size = pixmap->GetBufferSize();
  if (size.IsEmpty() || size.width() > 8192 || size.height() > 8192)
    return nullptr;
  auto* native = static_cast<OH_NativeBuffer*>(GetOhosNativeBuffer(*pixmap));
  if (!native)
    return nullptr;
  OH_NativeBuffer_Config config{};
  OH_NativeBuffer_GetConfig(native, &config);
  const int native_format = bgra ? NATIVEBUFFER_PIXEL_FMT_BGRA_8888
                                 : NATIVEBUFFER_PIXEL_FMT_RGBA_8888;
  const VkFormat vk_format = bgra ? VK_FORMAT_B8G8R8A8_UNORM
                                  : VK_FORMAT_R8G8B8A8_UNORM;
  if (config.width != size.width() || config.height != size.height() ||
      config.format != native_format)
    return nullptr;
  const VkDevice device = queue->GetVulkanDevice();
  VkNativeBufferFormatPropertiesOHOS format_properties{
      VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS};
  VkNativeBufferPropertiesOHOS properties{
      VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS};
  properties.pNext = &format_properties;
  VkResult result = GetQuery(device)(device, native, &properties);
  constexpr VkFormatFeatureFlags required_features =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  if (result != VK_SUCCESS || !properties.memoryTypeBits ||
      !properties.allocationSize || properties.allocationSize > 128 * 1024 * 1024 ||
      (format_properties.format != VK_FORMAT_UNDEFINED &&
       format_properties.format != vk_format) ||
      (format_properties.formatFeatures & required_features) != required_features) {
    LOG(ERROR) << "OHOS offscreen Vulkan: invalid native properties result="
               << result << " format=" << format_properties.format
               << " features=" << format_properties.formatFeatures;
    return nullptr;
  }
  VkExternalMemoryImageCreateInfo external{
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS;
  VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  create.pNext = &external;
  // Dawn can create both linear and sRGB views of this same native image.
  create.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  create.imageType = VK_IMAGE_TYPE_2D;
  create.format = vk_format;
  create.extent = {static_cast<uint32_t>(size.width()),
                   static_cast<uint32_t>(size.height()), 1};
  create.mipLevels = create.arrayLayers = 1;
  create.samples = VK_SAMPLE_COUNT_1_BIT;
  create.tiling = VK_IMAGE_TILING_OPTIMAL;
  create.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage image = VK_NULL_HANDLE;
  result = vkCreateImage(device, &create, nullptr, &image);
  if (result == VK_ERROR_INVALID_EXTERNAL_HANDLE ||
      result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
    external.handleTypes = 0x00002000;
    result = vkCreateImage(device, &create, nullptr, &image);
  }
  if (result != VK_SUCCESS) {
    LOG(ERROR) << "OHOS offscreen Vulkan: image creation failed " << result;
    return nullptr;
  }
  uint32_t type = 0;
  while (!(properties.memoryTypeBits & (1u << type)))
    ++type;
  VkImportNativeBufferInfoOHOS import{
      VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS};
  import.buffer = native;
  VkMemoryDedicatedAllocateInfo dedicated{
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.pNext = &import;
  dedicated.image = image;
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.pNext = &dedicated;
  allocate.allocationSize = properties.allocationSize;
  allocate.memoryTypeIndex = type;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  result = vkAllocateMemory(device, &allocate, nullptr, &memory);
  if (result == VK_SUCCESS)
    result = vkBindImageMemory(device, image, memory, 0);
  if (result != VK_SUCCESS) {
    LOG(ERROR) << "OHOS offscreen Vulkan: memory import failed " << result;
    vkDestroyImage(device, image, nullptr);
    if (memory)
      vkFreeMemory(device, memory, nullptr);
    return nullptr;
  }
  auto imported = gpu::VulkanImage::Create(
      queue, image, memory, size, create.format, create.tiling,
      properties.allocationSize, type, create.usage, create.flags);
  imported->set_queue_family_index(VK_QUEUE_FAMILY_FOREIGN_EXT);
  imported->set_native_pixmap(std::move(pixmap));
  return imported;
}

std::unique_ptr<gpu::VulkanImage> ImportOhosVideoVulkan(
    gpu::VulkanDeviceQueue* queue,
    gfx::GpuMemoryBufferHandle handle,
    const gfx::Size& size,
    const gfx::ColorSpace& color_space) {
  if (!CanImportOhosVideoVulkan(queue) || handle.type != gfx::NATIVE_PIXMAP)
    return nullptr;
  const auto& prefix = handle.native_pixmap_handle().ohos_buffer_prefix;
  if (prefix.size() != 14)
    return nullptr;
  viz::SharedImageFormat format;
  if (prefix[8] == NATIVEBUFFER_PIXEL_FMT_YCBCR_P010)
    format = viz::MultiPlaneFormat::kP010;
  else if (prefix[8] == NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP)
    format = viz::MultiPlaneFormat::kNV12;
  else
    return nullptr;
  format.SetPrefersExternalSampler();
  auto pixmap = ImportOhosVideoNativePixmap(
      size, format, std::move(handle).native_pixmap_handle());
  return ImportOhosVideoVulkan(queue, std::move(pixmap), color_space);
}

std::unique_ptr<gpu::VulkanImage> ImportOhosVideoVulkan(
    gpu::VulkanDeviceQueue* queue,
    scoped_refptr<gfx::NativePixmap> pixmap,
    const gfx::ColorSpace& color_space) {
  if (!CanImportOhosVideoVulkan(queue) || !pixmap ||
      !GetOhosVideoNativePixmapId(*pixmap).has_value())
    return nullptr;
  auto format = pixmap->GetSharedImageFormat();
  if (!format.PrefersExternalSampler())
    return nullptr;
  format.ClearPrefersExternalSampler();
  if (format != viz::MultiPlaneFormat::kP010 &&
      format != viz::MultiPlaneFormat::kNV12)
    return nullptr;
  const bool p010 = format == viz::MultiPlaneFormat::kP010;
  const VkFormat expected_format =
      p010 ? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
           : VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
  const auto size = pixmap->GetBufferSize();
  if (size.IsEmpty() || size.width() > 8192 || size.height() > 8192)
    return nullptr;
  auto* native = static_cast<OH_NativeBuffer*>(GetOhosNativeBuffer(*pixmap));
  if (!native)
    return nullptr;
  OH_NativeBuffer_Config config{};
  OH_NativeBuffer_GetConfig(native, &config);
  const int expected_native_format = p010 ? NATIVEBUFFER_PIXEL_FMT_YCBCR_P010
                                         : NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP;
  if (config.width != size.width() || config.height != size.height() ||
      config.format != expected_native_format)
    return nullptr;
  const VkDevice device = queue->GetVulkanDevice();
  VkNativeBufferFormatPropertiesOHOS native_format{
      VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS};
  VkNativeBufferPropertiesOHOS properties{
      VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS};
  properties.pNext = &native_format;
  VkResult result = GetQuery(device)(device, native, &properties);
  if (result != VK_SUCCESS || !properties.memoryTypeBits ||
      !properties.allocationSize ||
      properties.allocationSize > 128 * 1024 * 1024 ||
      !(native_format.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
    LOG(ERROR) << "OHOS video Vulkan: native properties rejected " << result;
    return nullptr;
  }
  // Check the native allocation's actual precision before handing it to Skia.
  if (native_format.format != VK_FORMAT_UNDEFINED &&
      native_format.format != expected_format) {
    LOG(ERROR) << "OHOS video Vulkan: unexpected native format "
               << native_format.format << " expected " << expected_format;
    return nullptr;
  }
  // Prefer the concrete multiplanar format. External-only formats require
  // the OHOS external-format YCbCr conversion extension in Skia as well.
  VkExternalFormatOHOS external_format{VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_OHOS};
  if (native_format.format == VK_FORMAT_UNDEFINED) {
    if (!native_format.externalFormat)
      return nullptr;
    external_format.externalFormat = native_format.externalFormat;
  }
  VkExternalMemoryImageCreateInfo external{
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  external.handleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS;
  external.pNext = external_format.externalFormat ? &external_format : nullptr;
  VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  create.pNext = &external;
  create.imageType = VK_IMAGE_TYPE_2D;
  create.format = native_format.format;
  create.extent = {static_cast<uint32_t>(size.width()),
                   static_cast<uint32_t>(size.height()), 1};
  create.mipLevels = create.arrayLayers = 1;
  create.samples = VK_SAMPLE_COUNT_1_BIT;
  create.tiling = VK_IMAGE_TILING_OPTIMAL;
  create.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  if (create.format != VK_FORMAT_UNDEFINED) {
    create.usage |=
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
  create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage image = VK_NULL_HANDLE;
  result = vkCreateImage(device, &create, nullptr, &image);
  if (result == VK_ERROR_INVALID_EXTERNAL_HANDLE ||
      result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
    external.handleTypes = 0x00002000;
    result = vkCreateImage(device, &create, nullptr, &image);
  }
  if (result != VK_SUCCESS) {
    LOG(ERROR) << "OHOS video Vulkan: create failed " << result
               << " format=" << native_format.format;
    return nullptr;
  }
  uint32_t type = 0;
  while (!(properties.memoryTypeBits & (1u << type)))
    ++type;
  VkImportNativeBufferInfoOHOS import{
      VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS};
  import.buffer = native;
  VkMemoryDedicatedAllocateInfo dedicated{
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.pNext = &import;
  dedicated.image = image;
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.pNext = &dedicated;
  allocate.allocationSize = properties.allocationSize;
  allocate.memoryTypeIndex = type;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  result = vkAllocateMemory(device, &allocate, nullptr, &memory);
  if (result == VK_SUCCESS)
    result = vkBindImageMemory(device, image, memory, 0);
  if (result != VK_SUCCESS) {
    LOG(ERROR) << "OHOS video Vulkan: bind failed " << result;
    vkDestroyImage(device, image, nullptr);
    if (memory)
      vkFreeMemory(device, memory, nullptr);
    return nullptr;
  }
  auto imported = gpu::VulkanImage::Create(
      queue, image, memory, size, create.format, create.tiling,
      properties.allocationSize, type, create.usage, 0);
  imported->set_queue_family_index(VK_QUEUE_FAMILY_FOREIGN_EXT);
  auto model = native_format.suggestedYcbcrModel;
  if (color_space.GetMatrixID() == gfx::ColorSpace::MatrixID::BT2020_NCL)
    model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
  else if (color_space.GetMatrixID() == gfx::ColorSpace::MatrixID::BT709)
    model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
  auto range = color_space.GetRangeID() == gfx::ColorSpace::RangeID::FULL
                   ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL
                   : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
  imported->set_ycbcr_info(gpu::VulkanYCbCrInfo(
      create.format, external_format.externalFormat, model, range,
      native_format.suggestedXChromaOffset,
      native_format.suggestedYChromaOffset, native_format.formatFeatures));
  imported->set_native_pixmap(std::move(pixmap));
  static bool logged = false;
  if (!logged) {
    logged = true;
    LOG(WARNING) << "OHOS video Vulkan: imported decoder buffer format="
                 << create.format
                 << " external=" << external_format.externalFormat
                 << " size=" << size.ToString();
  }
  return imported;
}
}  // namespace ui
