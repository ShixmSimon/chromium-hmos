// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Based on shared_image_backing_factory_ahardwarebuffer.cc originally written by
// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. 

#include "gpu/command_buffer/service/shared_image/ohos_native_buffer_image_backing_factory.h"

#include <dawn/native/DawnNative.h>
#include <dawn/webgpu.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>
#include <native_buffer/native_buffer.h>

#include "base/containers/flat_set.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/ohos/scoped_native_buffer_handle.h"
#include "base/posix/eintr_wrapper.h"
#include "build/build_config.h"
#include "gpu/command_buffer/service/vulkan_context_provider.h"
#include "gpu/command_buffer/common/gles2_cmd_utils.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/command_buffer/service/gles2_cmd_decoder.h"
#include "gpu/command_buffer/service/memory_tracking.h"
#include "gpu/command_buffer/service/ohos/native_buffer_config_impl.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#include "gpu/command_buffer/service/shared_image/gl_texture_ohos_image_representation.h"
#include "gpu/command_buffer/service/shared_image/gl_texture_passthrough_ohos_image_representation.h"
#include "gpu/command_buffer/service/shared_image/ohos_native_buffer_image_backing.h"
#include "gpu/command_buffer/service/shared_image/shared_image_backing.h"
#include "gpu/command_buffer/service/shared_image/shared_image_representation.h"
#include "gpu/command_buffer/service/shared_image/skia_gl_image_representation.h"
#include "gpu/command_buffer/service/shared_image/skia_vk_ohos_native_buffer_image_representation.h"
#include "gpu/command_buffer/service/skia_utils.h"
#include "gpu/command_buffer/service/texture_manager.h"
#include "gpu/vulkan/vulkan_image.h"
#include "third_party/bounds_checking_function/include/securec.h"
#include "third_party/skia/include/gpu/ganesh/vk/GrVkBackendSurface.h"
#include "third_party/skia/include/private/chromium/GrPromiseImageTexture.h"
#include "ui/gfx/buffer_format_util.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gl/buildflags.h"
#include "ui/gl/gl_context.h"
#include "ui/gl/gl_gl_api_implementation.h"
#include "ui/gl/gl_utils.h"
#include "ui/gl/gl_version_info.h"
#include "ui/gl/scoped_binders.h"
#include "ui/gl/scoped_egl_image.h"

namespace gpu {
namespace {
const size_t bitsPerPixel = 32;

constexpr viz::SharedImageFormat kSupportedFormats[6]{
    viz::SinglePlaneFormat::kRGBA_8888, viz::SinglePlaneFormat::kRGB_565,
    viz::SinglePlaneFormat::kBGR_565,   viz::SinglePlaneFormat::kRGBA_F16,
    viz::SinglePlaneFormat::kRGBX_8888, viz::SinglePlaneFormat::kRGBA_1010102};

// Returns whether the format is supported by NativeBuffer.
//LCOV_EXCL_START
bool NativeBufferSupportedFormat(viz::SharedImageFormat format) {
  return base::Contains(kSupportedFormats, format);
}
//LCOV_EXCL_STOP

int ConvertToNativeBufferFormat(viz::SharedImageFormat format) {
  if (format == viz::SinglePlaneFormat::kRGBA_8888) {
    return NATIVEBUFFER_PIXEL_FMT_RGBA_8888;
  } else if (format == viz::SinglePlaneFormat::kRGB_565) {
    return NATIVEBUFFER_PIXEL_FMT_RGB_565;
  } else if (format == viz::SinglePlaneFormat::kBGR_565) {
    return NATIVEBUFFER_PIXEL_FMT_BGR_565;
  } else if (format == viz::SinglePlaneFormat::kRGBA_F16) {
    return NATIVEBUFFER_PIXEL_FMT_RGBA16_FLOAT;
  } else if (format == viz::SinglePlaneFormat::kRGBX_8888) {
    return NATIVEBUFFER_PIXEL_FMT_RGBX_8888;
  } else if (format == viz::SinglePlaneFormat::kRGBA_1010102) {
    return NATIVEBUFFER_PIXEL_FMT_RGBA_1010102;
  }

  return NATIVEBUFFER_PIXEL_FMT_RGBA_8888;
}

constexpr SharedImageUsageSet kSupportedUsage =
    SHARED_IMAGE_USAGE_GLES2_READ | SHARED_IMAGE_USAGE_GLES2_WRITE |
    SHARED_IMAGE_USAGE_GLES2_FOR_RASTER_ONLY |
    SHARED_IMAGE_USAGE_DISPLAY_WRITE | SHARED_IMAGE_USAGE_DISPLAY_READ |
    SHARED_IMAGE_USAGE_RASTER_READ | SHARED_IMAGE_USAGE_RASTER_WRITE |
    SHARED_IMAGE_USAGE_RASTER_OVER_GLES2_ONLY |
    SHARED_IMAGE_USAGE_OOP_RASTERIZATION | SHARED_IMAGE_USAGE_SCANOUT |
    SHARED_IMAGE_USAGE_WEBGPU_READ | SHARED_IMAGE_USAGE_WEBGPU_WRITE |
    SHARED_IMAGE_USAGE_VIDEO_DECODE |
    SHARED_IMAGE_USAGE_WEBGPU_SWAP_CHAIN_TEXTURE |
    SHARED_IMAGE_USAGE_HIGH_PERFORMANCE_GPU |
    SHARED_IMAGE_USAGE_WEBGPU_STORAGE_TEXTURE |
    SHARED_IMAGE_USAGE_CONCURRENT_READ_WRITE;
}  // namespace

// static
OHOSNativeBufferImageBackingFactory::FormatInfo
OHOSNativeBufferImageBackingFactory::FormatInfoForSupportedFormat(
    viz::SharedImageFormat format,
    const gles2::Validators* validators,
    const GLFormatCaps& gl_format_caps) {
  CHECK(NativeBufferSupportedFormat(format));

  FormatInfo info;

  // In future when we use GL_TEXTURE_EXTERNAL_OES target
  // with NativeBuffer, we need to check if oes_egl_image_external is supported
  // or not.
  const bool is_egl_image_supported =
      gl::g_current_gl_driver->ext.b_GL_OES_EGL_image;
  if (!is_egl_image_supported) {
    return info;
  }

  // Check if NativeBuffer backed GL texture can be created using this format
  // and gather GL related format info. Add vulkan related information in
  // future.
  GLFormatDesc format_desc =
      gl_format_caps.ToGLFormatDescOverrideHalfFloatType(format,
                                                         /*plane_index=*/0);
  GLuint internal_format = format_desc.image_internal_format;
  GLenum gl_format = format_desc.data_format;
  GLenum gl_type = format_desc.data_type;

  // GLImageNativeBuffer supports internal format GL_RGBA and GL_RGB.
  if (internal_format != GL_RGBA && internal_format != GL_RGB &&
      internal_format != GL_RGBA16F) {
    return info;
  }

  // Validate if GL format, type and internal format is supported.
  if (validators->texture_internal_format.IsValid(internal_format) &&
      validators->texture_format.IsValid(gl_format) &&
      validators->pixel_type.IsValid(gl_type)) {
    info.gl_supported = true;
    info.gl_format = gl_format;
    info.gl_type = gl_type;
    info.internal_format = internal_format;
  }
  return info;
}

OHOSNativeBufferImageBackingFactory::OHOSNativeBufferImageBackingFactory(
    const gles2::FeatureInfo* feature_info,
    const GpuPreferences& gpu_preferences)
    : SharedImageBackingFactory(kSupportedUsage),
      use_passthrough_(gpu_preferences.use_passthrough_cmd_decoder &&
                       gl::PassthroughCommandDecoderSupported()),
      gl_format_caps_(GLFormatCaps(feature_info)) {
  // Build the feature info for all the supported formats.
  for (auto format : kSupportedFormats) {
    format_infos_[format] = FormatInfoForSupportedFormat(
        format, feature_info->validators(), gl_format_caps_);
  }

  // We are using below GL api calls for now as Vulkan mode
  // doesn't exist. Once we have vulkan support, we shouldn't query GL in this
  // code until we are asked to make a GL representation (or allocate a
  // backing for import into GL)? We may use a NativeBuffer exclusively
  // with Vulkan, where there is no need to require that a GL context is
  // current. Maybe we can lazy init this if someone tries to create a
  // NativeBuffer with SHARED_IMAGE_USAGE_GLES2 ||
  // !gpu_preferences.enable_vulkan. When in Vulkan mode, we should only need
  // this with GLES2.
  gl::GLApi* api = gl::g_current_gl_context;
  api->glGetIntegervFn(GL_MAX_TEXTURE_SIZE, &max_gl_texture_size_);

  // Ensure max_texture_size_ is less than INT_MAX so that gfx::Rect and friends
  // can be used to accurately represent all valid sub-rects, with overflow
  // cases, clamped to INT_MAX, always invalid.
  max_gl_texture_size_ = std::min(max_gl_texture_size_, INT_MAX - 1);

#if BUILDFLAG(USE_DAWN)
  // If building with Dawn support enabled, ensure that we have access to the
  // Dawn procs.
  dawn_procs_ = base::MakeRefCounted<base::RefCountedData<DawnProcTable>>(
      dawn::native::GetProcs());
#endif  // BUILDFLAG(USE_DAWN)
}

//LCOV_EXCL_START
OHOSNativeBufferImageBackingFactory::~OHOSNativeBufferImageBackingFactory() =
    default;
//LCOV_EXCL_STOP

bool OHOSNativeBufferImageBackingFactory::ValidateUsage(
    SharedImageUsageSet usage,
    const gfx::Size& size,
    viz::SharedImageFormat format) const {
  if (!NativeBufferSupportedFormat(format)) {
    LOG(ERROR) << "viz::SharedImageFormat " << format.ToString()
               << " not supported by NativeBuffer";
    return false;
  }

  // Check if NativeBuffer can be created with the current size restrictions.
  if (size.width() < 1 || size.height() < 1 ||
      size.width() > max_gl_texture_size_ ||
      size.height() > max_gl_texture_size_) {
    LOG(ERROR) << "CreateSharedImage: invalid size=" << size.ToString()
               << " max_gl_texture_size=" << max_gl_texture_size_;
    return false;
  }

  return true;
}

//LCOV_EXCL_START
// Stub implementations of some dependencies
bool ValidateUsage(SharedImageUsageSet usage,
                   const gfx::Size& size,
                   const viz::SharedImageFormat& format) {
  // Stub implementation: always return true
  return true;
}

size_t OHOSBitsPerPixel(const viz::SharedImageFormat& format) {
  // Stub implementation: return a fixed value
  return bitsPerPixel;
}

struct FormatInfo {
  int native_buffer_format;
};

FormatInfo GetFormatInfo(const viz::SharedImageFormat& format) {
  // Stub implementation: return dummy data
  return {0};
}
//LCOV_EXCL_STOP

std::unique_ptr<SharedImageBacking>
OHOSNativeBufferImageBackingFactory::MakeBacking(
    const Mailbox& mailbox,
    viz::SharedImageFormat format,
    const gfx::Size& size,
    const gfx::ColorSpace& color_space,
    GrSurfaceOrigin surface_origin,
    SkAlphaType alpha_type,
    SharedImageUsageSet usage,
    std::string debug_label,
    bool is_thread_safe,
    base::span<const uint8_t> pixel_data) {
  if (!ValidateUsage(usage, size, format)) {
    return nullptr;
  }

  // Calculate SharedImage size in bytes.
  auto estimated_size = format.MaybeEstimatedSizeInBytes(size);
  if (!estimated_size) {
    LOG(ERROR) << "Failed to calculate SharedImage size";
    return nullptr;
  }

  return MakeBackingWithValidateConfig(mailbox, format, size, color_space,
                                       surface_origin, alpha_type, usage,
                                       is_thread_safe, pixel_data);
}

std::unique_ptr<SharedImageBacking>
OHOSNativeBufferImageBackingFactory::MakeBackingWithValidateConfig(
    const Mailbox& mailbox,
    viz::SharedImageFormat format,
    const gfx::Size& size,
    const gfx::ColorSpace& color_space,
    GrSurfaceOrigin surface_origin,
    SkAlphaType alpha_type,
    SharedImageUsageSet usage,
    bool is_thread_safe,
    base::span<const uint8_t> pixel_data) {
  // Setup NativeBuffer.
  void* buffer = nullptr;
  std::shared_ptr<OHOS::NWeb::NativeBufferConfigAdapterImpl> configAdapter =
      std::make_shared<OHOS::NWeb::NativeBufferConfigAdapterImpl>();
  configAdapter->SetBufferWidth(size.width());
  configAdapter->SetBufferHeight(size.height());
  configAdapter->SetBufferFormat(ConvertToNativeBufferFormat(format));
  configAdapter->SetBufferUsage(
      gpu::OH_NativeBuffer_Usage::NATIVEBUFFER_USAGE_HW_RENDER |
      gpu::OH_NativeBuffer_Usage::NATIVEBUFFER_USAGE_HW_TEXTURE |
      gpu::OH_NativeBuffer_Usage::NATIVEBUFFER_USAGE_MEM_DMA |
      gpu::OH_NativeBuffer_Usage::NATIVEBUFFER_USAGE_CPU_READ |
      gpu::OH_NativeBuffer_Usage::NATIVEBUFFER_USAGE_CPU_WRITE);

  // Allocate a NativeBuffer.
  OHOS::NWeb::OhosNativeBufferAdapter& adapter =
      OHOS::NWeb::OhosAdapterHelper::GetInstance().GetOhosNativeBufferAdapter();
  adapter.Allocate(configAdapter, &buffer);

  if (buffer == nullptr) {
    LOG(ERROR) << "Failed to allocate NativeBuffer";
    return nullptr;
  }
  LOG(INFO) << "NativeBuffer created, width: " << size.width() << " height: " << size.height();
  auto handle = gpu::ScopedNativeBufferHandle::Adopt(buffer);

  base::ScopedFD initial_upload_fd;
  // Upload data if necessary
  if (!pixel_data.empty()) {
    // Get description about buffer to obtain stride
    std::shared_ptr<OHOS::NWeb::NativeBufferConfigAdapterImpl>
        configAdapterTmp =
            std::make_shared<OHOS::NWeb::NativeBufferConfigAdapterImpl>();
    OHOS::NWeb::OhosAdapterHelper::GetInstance()
        .GetOhosNativeBufferAdapter()
        .Describe(configAdapterTmp, buffer);

    void* address = nullptr;
    if (int error =
            OHOS::NWeb::OhosAdapterHelper::GetInstance()
                .GetOhosNativeBufferAdapter()
                .Lock(buffer,
                      gpu::OH_NativeBuffer_Usage::NATIVEBUFFER_USAGE_CPU_WRITE,
                      -1, &address)) {
      LOG(ERROR) << "Failed to lock NativeBuffer: " << error;
      return nullptr;
    }

    int bytes_per_pixel = format.BitsPerPixel() / 8;

    // NOTE: hwb_info.stride is in pixels
    int dst_stride = configAdapterTmp->GetBufferStride();
    int src_stride = bytes_per_pixel * size.width();

    size_t expected_size = src_stride * size.height();
    if (pixel_data.size() != expected_size) {
        LOG(ERROR) << "Invalid initial pixel data size: expected " 
                   << expected_size << ", got " << pixel_data.size();
        return nullptr;
    }

    for (int y = 0; y < size.height(); y++) {
      void* dst = reinterpret_cast<uint8_t*>(address) + dst_stride * y;
      const void* src = pixel_data.data() + src_stride * y;
      if (memcpy_s(dst, dst_stride, src, src_stride) != EOK) {
        LOG(ERROR) << "memcpy_s failed:(OHOSNativeBufferImageBackingFactory::MakeBackingWithValidateConfig)";
      }
    }

    int32_t fence = -1;
    OHOS::NWeb::OhosAdapterHelper::GetInstance()
        .GetOhosNativeBufferAdapter()
        .Unlock(buffer, &fence);
    initial_upload_fd = base::ScopedFD(fence);
  }

  // Calculate SharedImage size in bytes.
  auto estimated_size = format.MaybeEstimatedSizeInBytes(size);
  if (!estimated_size) {
    LOG(ERROR) << "Failed to calculate SharedImage size";
    return nullptr;
  }

  // Create the OHOSNativeBufferImageBacking object
  auto backing = std::make_unique<OhosNativeBufferImageBacking>(
      mailbox, format, size, color_space, surface_origin, alpha_type, usage,
      /*debug_label=*/"OhosNativeBuffer", std::move(handle),
      estimated_size.value() /*estimated_size*/, is_thread_safe, std::move(initial_upload_fd),
      false /*use_passthrough_*/, gl_format_caps_);

  // If we uploaded initial data, set the backing as cleared.
  if (!pixel_data.empty()) {
    backing->SetCleared();
  }

  if (backing) {
    LOG(INFO) << "NativeBuffer backing created:(OhosNativeBufferImageBacking), and mailbox is: " << mailbox.ToDebugString();
  }

  return backing;
}

//LCOV_EXCL_START
std::unique_ptr<SharedImageBacking>
OHOSNativeBufferImageBackingFactory::CreateSharedImage(
    const Mailbox& mailbox,
    viz::SharedImageFormat format,
    SurfaceHandle surface_handle,
    const gfx::Size& size,
    const gfx::ColorSpace& color_space,
    GrSurfaceOrigin surface_origin,
    SkAlphaType alpha_type,
    SharedImageUsageSet usage,
    std::string debug_label,
    bool is_thread_safe) {
  return MakeBacking(mailbox, format, size, color_space, surface_origin,
                     alpha_type, usage, std::move(debug_label), is_thread_safe,
                     base::span<uint8_t>());
}

std::unique_ptr<SharedImageBacking>
OHOSNativeBufferImageBackingFactory::CreateSharedImage(
    const Mailbox& mailbox,
    viz::SharedImageFormat format,
    const gfx::Size& size,
    const gfx::ColorSpace& color_space,
    GrSurfaceOrigin surface_origin,
    SkAlphaType alpha_type,
    SharedImageUsageSet usage,
    std::string debug_label,
    bool is_thread_safe,
    base::span<const uint8_t> pixel_data) {
  return MakeBacking(mailbox, format, size, color_space, surface_origin,
                     alpha_type, usage, std::move(debug_label), is_thread_safe,
                     pixel_data);
}

bool OHOSNativeBufferImageBackingFactory::CanImportGpuMemoryBuffer(
    gfx::GpuMemoryBufferType memory_buffer_type) {
  return memory_buffer_type == gfx::OHOS_NATIVE_BUFFER;
}
//LCOV_EXCL_STOP

bool OHOSNativeBufferImageBackingFactory::IsSupported(
    SharedImageUsageSet usage,
    viz::SharedImageFormat format,
    const gfx::Size& size,
    bool thread_safe,
    gfx::GpuMemoryBufferType gmb_type,
    GrContextType gr_context_type,
    base::span<const uint8_t> pixel_data) {
  LOG(DEBUG)
      << "CreateSharedImage OHOSNativeBufferImageBackingFactory::IsSupported ";
  LOG(DEBUG) << "usage: " << usage;
  LOG(DEBUG) << "format: " << format.ToString();
  LOG(DEBUG) << "size: " << size.width() << "x" << size.height();
  LOG(DEBUG) << "thread_safe: " << thread_safe;
  LOG(DEBUG) << "gmb_type: " << gmb_type;
  LOG(DEBUG) << "pixel_data: " << pixel_data.size();

  if (gmb_type == gfx::NATIVE_PIXMAP) {
    return false;
  }

  if (format.is_multi_plane()) {
    return false;
  }

  if (gmb_type != gfx::EMPTY_BUFFER && !CanImportGpuMemoryBuffer(gmb_type)) {
    return false;
  }

  if (!NativeBufferSupportedFormat(format)) {
    return false;
  }

  return true;
}

//LCOV_EXCL_START
OHOSNativeBufferImageBackingFactory::FormatInfo::FormatInfo() = default;

OHOSNativeBufferImageBackingFactory::FormatInfo::~FormatInfo() = default;
//LCOV_EXCL_STOP

std::unique_ptr<SharedImageBacking>
OHOSNativeBufferImageBackingFactory::CreateSharedImage(
    const Mailbox& mailbox,
    viz::SharedImageFormat format,
    const gfx::Size& size,
    const gfx::ColorSpace& color_space,
    GrSurfaceOrigin surface_origin,
    SkAlphaType alpha_type,
    SharedImageUsageSet usage,
    std::string debug_label,
    gfx::GpuMemoryBufferHandle handle) {
  if (!ValidateUsage(usage, size, format)) {
    return nullptr;
  }

  auto estimated_size = format.MaybeEstimatedSizeInBytes(size);
  if (!estimated_size) {
    LOG(ERROR) << "Failed to calculate SharedImage size";
    return nullptr;
  }

  auto backing = std::make_unique<OhosNativeBufferImageBacking>(
      mailbox, format, size, color_space, surface_origin, alpha_type, usage,
      std::move(debug_label), std::move(handle.ohos_hardware_buffer),
      estimated_size.value(), false, base::ScopedFD(), use_passthrough_,
      gl_format_caps_);

  backing->SetCleared();
  return backing;
}

//LCOV_EXCL_START
SharedImageBackingType OHOSNativeBufferImageBackingFactory::GetBackingType() {
  return SharedImageBackingType::kOHOSNativeBuffer;
}
//LCOV_EXCL_STOP

}  // namespace gpu
