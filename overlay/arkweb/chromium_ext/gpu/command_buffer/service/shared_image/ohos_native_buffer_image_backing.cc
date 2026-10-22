// Copyright (c) 2025 Huawei Device Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/command_buffer/service/shared_image/ohos_native_buffer_image_backing.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "base/logging.h"
#include "base/ohos/scoped_native_buffer_handle.h"
#include "base/posix/eintr_wrapper.h"
#include "gpu/command_buffer/service/vulkan_context_provider.h"
#include "gpu/command_buffer/common/gles2_cmd_utils.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/command_buffer/service/gles2_cmd_decoder.h"
#include "gpu/command_buffer/service/memory_tracking.h"
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
#include "third_party/skia/include/gpu/ganesh/vk/GrVkBackendSurface.h"
#include "third_party/skia/include/gpu/vk/VulkanMutableTextureState.h"
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

#define EGL_NATIVE_BUFFER_OHOS 0x34E1

namespace gpu {
//LCOV_EXCL_START
void ShowError() {
  EGLint error = eglGetError();
  switch (error) {
    case EGL_BAD_DISPLAY:
      LOG(ERROR) << "eglCreateImageKHR failed: EGL_BAD_DISPLAY";
      break;
    case EGL_BAD_CONTEXT:
      LOG(ERROR) << "eglCreateImageKHR failed: EGL_BAD_CONTEXT";
      break;
    case EGL_BAD_PARAMETER:
      LOG(ERROR) << "eglCreateImageKHR failed: EGL_BAD_PARAMETER";
      break;
    case EGL_BAD_MATCH:
      LOG(ERROR) << "eglCreateImageKHR failed: EGL_BAD_MATCH";
      break;
    default:
      LOG(ERROR) << "eglCreateImageKHR failed: Unknown error (" << error << ")";
      break;
  }
}

std::unique_ptr<VulkanImage> CreateVkImageFromNativeBufferHandle(
    gpu::ScopedNativeBufferHandle nb_handle,
    SharedContextState* context_state,
    const gfx::Size& size,
    const viz::SharedImageFormat& format,
    uint32_t queue_family_index) {
  DCHECK(context_state);
  DCHECK(context_state->GrContextIsVulkan());
  auto* device_queue = context_state->vk_context_provider()->GetDeviceQueue();
  gfx::GpuMemoryBufferHandle gmb_handle(std::move(nb_handle));
  return VulkanImage::CreateFromGpuMemoryBufferHandle(
      device_queue, std::move(gmb_handle), size, ToVkFormatSinglePlanar(format),
      /*usage=*/0, /*flags=*/0, /*image_tiling=*/VK_IMAGE_TILING_OPTIMAL,
      /*queue_family_index=*/queue_family_index);
}
//LCOV_EXCL_STOP

// Move to a helper class.
gl::ScopedEGLImage CreateEGLImageFromNativeBuffer(OHOSNativeBuffer buffer) {
  EGLint egl_image_attribs[] = {
      EGL_IMAGE_PRESERVED,
      EGL_TRUE,
      EGL_NONE,
  };  // Start with minimal attributes

  if (buffer == nullptr) {
    LOG(ERROR) << "CreateEGLImageFromNativeBuffer: rawbuffer is nullptr";
    return gl::ScopedEGLImage();
  }

  EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (display == EGL_NO_DISPLAY) {
    EGLint error = eglGetError();
    LOG(ERROR) << "Failed to get default EGL display, error: " << error;
    return gl::ScopedEGLImage();
  }

  if (!eglInitialize(display, nullptr, nullptr)) {
    EGLint error = eglGetError();
    LOG(ERROR) << "Failed to initialize EGL, error: " << error;
    return gl::ScopedEGLImage();
  }

  const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
  if (!extensions) {
    EGLint error = eglGetError();
    LOG(ERROR) << "Failed to query EGL extensions, error: " << error;
    return gl::ScopedEGLImage();
  }

  if (!strstr(extensions, "EGL_KHR_image_base") ||
      !strstr(extensions, "EGL_KHR_image")) {
    LOG(ERROR) << "Required EGL extensions are not supported";
  }

  EGLClientBuffer eglBuffer;
  int opstatus = OHOS::NWeb::OhosAdapterHelper::GetInstance()
                     .GetOhosNativeBufferAdapter()
                     .GetEGLBuffer(buffer, &eglBuffer);
  if (opstatus != 0) {
    LOG(ERROR) << "Failed to getEGLBuffer!";
    return gl::ScopedEGLImage();
  }

  EGLImageKHR egl_image =
      eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_OHOS,
                        eglBuffer, egl_image_attribs);
  if (egl_image == EGL_NO_IMAGE_KHR) {
    ShowError();
    return gl::ScopedEGLImage();
  }
  OHOS::NWeb::OhosAdapterHelper::GetInstance()
      .GetOhosNativeBufferAdapter()
      .FreeEGLBuffer(eglBuffer);

  return gl::ScopedEGLImage(egl_image);
}

GLuint CreateAndBindTexture(EGLImage image, GLenum target) {
  gl::GLApi* api = gl::g_current_gl_context;
  GLuint service_id = 0;
  api->glGenTexturesFn(1, &service_id);
  gl::ScopedTextureBinder texture_binder(target, service_id);

  api->glTexParameteriFn(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  api->glTexParameteriFn(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  api->glTexParameteriFn(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  api->glTexParameteriFn(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glEGLImageTargetTexture2DOES(target, image);

  return service_id;
}

// Vk backed Skia representation of AHardwareBufferImageBacking.
//LCOV_EXCL_START
class SkiaVkNBRepresentation : public SkiaVkNBImageRepresentation {
 public:
  SkiaVkNBRepresentation(SharedImageManager* manager,
                         OhosNativeBufferImageBacking* backing,
                         scoped_refptr<SharedContextState> context_state,
                         std::unique_ptr<VulkanImage> vulkan_image,
                         MemoryTypeTracker* tracker)
      : SkiaVkNBImageRepresentation(manager,
                                    backing,
                                    std::move(context_state),
                                    tracker) {
    DCHECK(vulkan_image);
    LOG(DEBUG)
        << "create vk native buffer image skia representation VulkanImage "
        << vulkan_image;

    vulkan_image_ = std::move(vulkan_image);
    // TODO(bsalomon): Determine whether it makes sense to attempt to reuse this
    // if the vk_info stays the same on subsequent calls.
    promise_texture_ = GrPromiseImageTexture::Make(GrBackendTextures::MakeVk(
        size().width(), size().height(),
        CreateGrVkImageInfo(vulkan_image_.get(), format(), color_space())));
    DCHECK(promise_texture_);
  }
};

OhosNativeBufferImageBacking::OhosNativeBufferImageBacking(
    const Mailbox& mailbox,
    viz::SharedImageFormat format,
    const gfx::Size& size,
    const gfx::ColorSpace& color_space,
    GrSurfaceOrigin surface_origin,
    SkAlphaType alpha_type,
    SharedImageUsageSet usage,
    std::string debug_label,
    size_t estimated_size,
    bool is_thread_safe,
    base::ScopedFD initial_upload_fd,
    const GLFormatCaps& gl_format_caps)
    : OhosImageBacking(mailbox,
                       format,
                       size,
                       color_space,
                       surface_origin,
                       alpha_type,
                       usage,
                       std::move(debug_label),
                       estimated_size,
                       is_thread_safe,
                       std::move(initial_upload_fd)),
      use_passthrough_(false),
      drdc_lock_helper_(base::MakeRefCounted<gpu::RefCountedLock>()),
      gl_format_caps_(gl_format_caps) {}

OhosNativeBufferImageBacking::OhosNativeBufferImageBacking(
    const Mailbox& mailbox,
    viz::SharedImageFormat format,
    const gfx::Size& size,
    const gfx::ColorSpace& color_space,
    GrSurfaceOrigin surface_origin,
    SkAlphaType alpha_type,
    SharedImageUsageSet usage,
    std::string debug_label,
    gpu::ScopedNativeBufferHandle handle,
    size_t estimated_size,
    bool is_thread_safe,
    base::ScopedFD initial_upload_fd,
    bool use_passthrough,
    const GLFormatCaps& gl_format_caps)
    : OhosImageBacking(mailbox,
                       format,
                       size,
                       color_space,
                       surface_origin,
                       alpha_type,
                       usage,
                       std::move(debug_label),
                       estimated_size,
                       is_thread_safe,
                       std::move(initial_upload_fd)),
      native_buffer_handle_(std::move(handle)),
      use_passthrough_(use_passthrough),
      drdc_lock_helper_(base::MakeRefCounted<gpu::RefCountedLock>()),
      gl_format_caps_(gl_format_caps) {}

OhosNativeBufferImageBacking::~OhosNativeBufferImageBacking() {
  // Locking here in destructor since we are accessing member variable
  // |have_context_| via have_context().
  AutoLock auto_lock(this);
  DCHECK(native_buffer_handle_.is_valid());
}

SharedImageBackingType OhosNativeBufferImageBacking::GetType() const {
  return SharedImageBackingType::kOHOSNativeBuffer;
}

gfx::Rect OhosNativeBufferImageBacking::ClearedRect() const {
  AutoLock auto_lock(this);
  return ClearedRectInternal();
}

void OhosNativeBufferImageBacking::SetClearedRect(
    const gfx::Rect& cleared_rect) {
  AutoLock auto_lock(this);
  SetClearedRectInternal(cleared_rect);
}

void OhosNativeBufferImageBacking::Update(
    std::unique_ptr<gfx::GpuFence> in_fence) {
  DCHECK(!in_fence);
}

gpu::ScopedNativeBufferHandle
OhosNativeBufferImageBacking::GetNativeBufferHandle() const {
  return native_buffer_handle_.Clone();
}
//LCOV_EXCL_STOP

std::unique_ptr<GLTextureImageRepresentation>
OhosNativeBufferImageBacking::ProduceGLTexture(SharedImageManager* manager,
                                               MemoryTypeTracker* tracker) {
  base::AutoLockMaybe auto_lock(drdc_lock_helper_.GetDrDcLockPtr());
  // Use same texture for all the texture representations generated from same
  // backing.
  DCHECK(native_buffer_handle_.is_valid());

  auto egl_image = CreateEGLImageFromNativeBuffer(native_buffer_handle_.get());
  if (!egl_image.is_valid()) {
    LOG(ERROR) << "NATIVE_BUFFER ProduceGLTexture err";
    return nullptr;
  }

  // Right GL format for RGBX NativeBuffer is GL_RGB8, so we don't use angle
  // rgbx.
  auto gl_format_desc =
      gl_format_caps_.ToGLFormatDescOverrideHalfFloatType(format(),
                                                          /*plane_index=*/0);
  GLuint service_id =
      CreateAndBindTexture(egl_image.get(), gl_format_desc.target);

  auto* texture =
      gles2::CreateGLES2TextureWithLightRef(service_id, gl_format_desc.target);
  texture->SetLevelInfo(gl_format_desc.target, 0,
                        gl_format_desc.image_internal_format, size().width(),
                        size().height(), 1, 0, gl_format_desc.data_format,
                        gl_format_desc.data_type, ClearedRect());
  texture->SetImmutable(true, false);

  return std::make_unique<GLTextureOhosImageRepresentation>(
      manager, this, tracker, std::move(egl_image), std::move(texture));
}

std::unique_ptr<GLTexturePassthroughImageRepresentation>
OhosNativeBufferImageBacking::ProduceGLTexturePassthrough(
    SharedImageManager* manager,
    MemoryTypeTracker* tracker) {
  base::AutoLockMaybe auto_lock(drdc_lock_helper_.GetDrDcLockPtr());
  // Use same texture for all the texture representations generated from same
  // backing.
  DCHECK(native_buffer_handle_.is_valid());

  LOG(ERROR) << "NATIVE_BUFFER ProduceGLTexturePassthrough";

  auto egl_image = CreateEGLImageFromNativeBuffer(native_buffer_handle_.get());
  if (!egl_image.is_valid()) {
    LOG(ERROR) << "NATIVE_BUFFER " << __PRETTY_FUNCTION__ << " :: " << __LINE__
               << " !egl_image.is_valid()\n";
    return nullptr;
  }

  // Right GL format for RGBX NativeBuffer is GL_RGB8, so we don't use angle
  // rgbx.
  auto gl_format_desc =
      gl_format_caps_.ToGLFormatDescOverrideHalfFloatType(format(),
                                                          /*plane_index=*/0);
  GLuint service_id =
      CreateAndBindTexture(egl_image.get(), gl_format_desc.target);

  auto texture = base::MakeRefCounted<gles2::TexturePassthrough>(
      service_id, gl_format_desc.target);
  texture->SetEstimatedSize(GetEstimatedSize());

  return std::make_unique<GLTexturePassthroughOhosImageRepresentation>(
      manager, this, tracker, std::move(egl_image), std::move(texture));
}

std::unique_ptr<SkiaGaneshImageRepresentation>
OhosNativeBufferImageBacking::ProduceSkiaGanesh(
    SharedImageManager* manager,
    MemoryTypeTracker* tracker,
    scoped_refptr<SharedContextState> context_state) {
  base::AutoLockMaybe auto_lock(drdc_lock_helper_.GetDrDcLockPtr());
  DCHECK(context_state);
  // Check whether we are in Vulkan mode OR GL mode and accordingly create
  // Skia representation.
#if BUILDFLAG(ENABLE_VULKAN)
  if (context_state->GrContextIsVulkan()) {
    uint32_t queue_family = VK_QUEUE_FAMILY_EXTERNAL;
    if (usage() & SHARED_IMAGE_USAGE_SCANOUT) {
      // Any OHOS API that consume or produce buffers requires a foreign queue.
      queue_family = VK_QUEUE_FAMILY_FOREIGN_EXT;
    }
    auto vulkan_image = CreateVkImageFromNativeBufferHandle(
        GetNativeBufferHandle(), context_state.get(), size(), format(),
        queue_family);
    if (!vulkan_image) {
      LOG(ERROR) << "create vk native buffer image backing fail";
      return nullptr;
    }

    return std::make_unique<SkiaVkNBRepresentation>(
        manager, this, std::move(context_state), std::move(vulkan_image),
        tracker);
  }
#endif

  DCHECK(context_state->GrContextIsGL());
  DCHECK(native_buffer_handle_.is_valid());

  std::unique_ptr<GLTextureImageRepresentationBase> gl_representation;
  if (use_passthrough_) {
    gl_representation = ProduceGLTexturePassthrough(manager, tracker);
  } else {
    gl_representation = ProduceGLTexture(manager, tracker);
  }

  if (!gl_representation) {
    LOG(ERROR) << "Unable produce gl texture!";
    return nullptr;
  }
  return SkiaGLImageRepresentation::Create(std::move(gl_representation),
                                           std::move(context_state), manager,
                                           this, tracker);
}
}  // namespace gpu
