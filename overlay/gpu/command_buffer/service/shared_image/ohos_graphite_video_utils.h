// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef GPU_COMMAND_BUFFER_SERVICE_SHARED_IMAGE_OHOS_GRAPHITE_VIDEO_UTILS_H_
#define GPU_COMMAND_BUFFER_SERVICE_SHARED_IMAGE_OHOS_GRAPHITE_VIDEO_UTILS_H_

#include <optional>
#include <utility>
#include <vulkan/vulkan.h>
#include "gpu/command_buffer/service/dawn_context_provider.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#include "gpu/vulkan/vulkan_ycbcr_info.h"
#include "ui/ozone/platform/ohos/ohos_native_pixmap.h"  // nogncheck
#include "ui/ozone/platform/ohos/ohos_video_graphite_metadata.h"  // nogncheck

namespace gpu {
inline bool SupportsOhosGraphiteVideo(SharedContextState* context) {
  if (!context || context->context_lost() || !context->IsGraphiteDawnVulkan())
    return false;
  const auto device = context->dawn_context_provider()->GetDevice();
  return device.HasFeature(wgpu::FeatureName::SharedTextureMemoryAHardwareBuffer) &&
         device.HasFeature(wgpu::FeatureName::SharedFenceSyncFD) &&
         device.HasFeature(wgpu::FeatureName::OpaqueYCbCrAndroidForExternalTexture) &&
         device.HasFeature(wgpu::FeatureName::YCbCrVulkanSamplers) &&
         device.HasFeature(wgpu::FeatureName::StaticSamplers);
}

inline ui::OhosVideoGraphiteMetadata OhosGraphiteVideoMetadata(
    const VulkanYCbCrInfo& info, const gfx::Size& size) {
  return {{{ui::OhosVideoGraphiteMetadata::kMagic, 1,
            static_cast<uint32_t>(size.width()), static_cast<uint32_t>(size.height()),
            info.image_format, static_cast<uint32_t>(info.external_format),
            static_cast<uint32_t>(info.external_format >> 32),
            info.suggested_ycbcr_model, info.suggested_ycbcr_range,
            info.suggested_xchroma_offset, info.suggested_ychroma_offset,
            info.format_features}}};
}

struct OhosGraphiteVideoImport {
  wgpu::SharedTextureMemory memory;
  VulkanYCbCrInfo info;
};

// Query the actual allocation on the active Dawn device. Never create a
// second Vulkan device in the browser, or infer external-format IDs from P010.
inline std::optional<OhosGraphiteVideoImport> ImportOhosGraphiteVideo(
    SharedContextState* context,
    const gfx::NativePixmap& pixmap,
    const gfx::ColorSpace& color_space) {
  if (!SupportsOhosGraphiteVideo(context))
    return std::nullopt;
  wgpu::SharedTextureMemoryAHardwareBufferDescriptor native{};
  native.handle = ui::GetOhosNativeBuffer(pixmap);
  if (!native.handle)
    return std::nullopt;
  wgpu::SharedTextureMemoryDescriptor descriptor{};
  descriptor.label = "OhosGraphiteDecoderNativeBuffer";
  descriptor.nextInChain = &native;
  auto memory = context->dawn_context_provider()->GetDevice()
                    .ImportSharedTextureMemory(&descriptor);
  wgpu::SharedTextureMemoryAHardwareBufferProperties native_properties{};
  wgpu::SharedTextureMemoryProperties properties{};
  properties.nextInChain = &native_properties;
  if (!memory || memory.GetProperties(&properties) != wgpu::Status::Success ||
      properties.format != wgpu::TextureFormat::OpaqueYCbCrAndroid ||
      properties.size.width != static_cast<uint32_t>(pixmap.GetBufferSize().width()) ||
      properties.size.height != static_cast<uint32_t>(pixmap.GetBufferSize().height()))
    return std::nullopt;
  const auto& yuv = native_properties.yCbCrInfo;
  // VulkanYCbCrInfo cannot transport arbitrary component swizzles. Refuse
  // such allocations instead of promising a different sampler to Viz.
  if ((yuv.vkComponentSwizzleRed != VK_COMPONENT_SWIZZLE_IDENTITY &&
       yuv.vkComponentSwizzleRed != VK_COMPONENT_SWIZZLE_R) ||
      (yuv.vkComponentSwizzleGreen != VK_COMPONENT_SWIZZLE_IDENTITY &&
       yuv.vkComponentSwizzleGreen != VK_COMPONENT_SWIZZLE_G) ||
      (yuv.vkComponentSwizzleBlue != VK_COMPONENT_SWIZZLE_IDENTITY &&
       yuv.vkComponentSwizzleBlue != VK_COMPONENT_SWIZZLE_B) ||
      (yuv.vkComponentSwizzleAlpha != VK_COMPONENT_SWIZZLE_IDENTITY &&
       yuv.vkComponentSwizzleAlpha != VK_COMPONENT_SWIZZLE_A) ||
      (!yuv.vkFormat && !yuv.externalFormat))
    return std::nullopt;
  uint32_t model = yuv.vkYCbCrModel;
  switch (color_space.GetMatrixID()) {
    case gfx::ColorSpace::MatrixID::BT709:
      model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
      break;
    case gfx::ColorSpace::MatrixID::BT2020_NCL:
      model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
      break;
    case gfx::ColorSpace::MatrixID::SMPTE170M:
    case gfx::ColorSpace::MatrixID::BT470BG:
      model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
      break;
    default:
      // Unspecified stream metadata keeps the allocation's driver suggestion.
      if (model < VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709 ||
          model > VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020)
        return std::nullopt;
      break;
  }
  const uint32_t range = color_space.GetRangeID() == gfx::ColorSpace::RangeID::FULL
                            ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL
                            : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
  uint32_t features = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  if (yuv.vkChromaFilter == wgpu::FilterMode::Linear)
    features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT;
  return OhosGraphiteVideoImport{std::move(memory), VulkanYCbCrInfo(
      yuv.vkFormat, yuv.externalFormat, model, range, yuv.vkXChromaOffset,
      yuv.vkYChromaOffset, features)};
}
}  // namespace gpu
#endif
