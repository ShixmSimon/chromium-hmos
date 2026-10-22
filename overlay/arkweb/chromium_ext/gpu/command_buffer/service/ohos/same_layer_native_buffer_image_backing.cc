// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Based on shared_image_video_image_reader.cc originally written by
// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. 

#include "gpu/command_buffer/service/ohos/same_layer_native_buffer_image_backing.h"

#include <sys/poll.h>

#include "base/logging.h"
#include "base/task/bind_post_task.h"
#include "gpu/command_buffer/service/vulkan_context_provider.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/command_buffer/service/abstract_texture_ohos.h"
#include "gpu/command_buffer/service/gles2_cmd_decoder.h"
#include "gpu/command_buffer/service/memory_tracking.h"
#include "gpu/command_buffer/service/ohos/native_buffer_config_impl.h"
#include "gpu/command_buffer/service/ohos/scoped_native_buffer_fence_sync.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#include "gpu/command_buffer/service/shared_image/shared_image_backing.h"
#include "gpu/command_buffer/service/shared_image/shared_image_representation.h"
#include "gpu/command_buffer/service/shared_image/skia_gl_image_representation.h"
#include "gpu/command_buffer/service/shared_image/skia_vk_ohos_native_buffer_image_representation.h"
#include "gpu/command_buffer/service/skia_utils.h"
#include "gpu/command_buffer/service/stream_texture_shared_image_interface.h"
#include "gpu/command_buffer/service/texture_manager.h"
#include "gpu/vulkan/vulkan_image.h"
#include "third_party/skia/include/gpu/ganesh/vk/GrVkBackendSurface.h"
#include "third_party/skia/include/gpu/vk/VulkanMutableTextureState.h"
#include "third_party/skia/include/private/chromium/GrPromiseImageTexture.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/gpu_fence_handle.h"
#include "ui/gl/android/egl_fence_utils.h"
#include "ui/gl/ohos/native_buffer_utils.h"
#include "ui/gl/scoped_restore_texture.h"

namespace gpu {

//LCOV_EXCL_START
class SkiaVkNBRepresentation : public SkiaVkNBImageRepresentation {
 public:
  SkiaVkNBRepresentation(SharedImageManager* manager,
                         SameLayerNativeBufferImageBacking* backing,
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
//LCOV_EXCL_STOP

namespace {
//LCOV_EXCL_START
std::unique_ptr<VulkanImage> CreateVkImageFromNativeBufferHandle(
    gpu::ScopedNativeBufferHandle nb_handle,
    SharedContextState* context_state,
    const gfx::Size& size,
    const viz::SharedImageFormat& format,
    uint32_t queue_family_index) {
  TRACE_EVENT2("base", __FILE__, "func", __func__, "line", __LINE__);
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

void CreateAndBindEglImageFromNativeBuffer(OHOSNativeBuffer buffer,
                                           GLuint service_id) {
  DCHECK(buffer);

  EGLClientBuffer egl_client_buffer;
  if (gl::ohos::GetEGLClientBufferFromNativeBuffer(buffer,
                                                   &egl_client_buffer) != 0) {
    LOG(ERROR) << "Failed to get EGLClientBuffer!";
    return;
  }

  auto egl_image = gl::ohos::CreateEGLImage(egl_client_buffer);
  gl::ohos::FreeEGLClientBuffer(egl_client_buffer);
  if (egl_image == EGL_NO_IMAGE_KHR) {
    LOG(ERROR) << "Failed to create EGLImage! ";
    return;
  }
  {
    // We should never alter gl binding without updating state tracking, which
    // we can't do here, so restore previous after we done.
    gl::ScopedRestoreTexture scoped_restore(gl::g_current_gl_context,
                                            GL_TEXTURE_EXTERNAL_OES);

    glBindTexture(GL_TEXTURE_EXTERNAL_OES, service_id);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, egl_image.get());
  }
}
}  // namespace

//LCOV_EXCL_START
SameLayerNativeBufferImageBacking::SameLayerNativeBufferImageBacking(
    const Mailbox& mailbox,
    const gfx::Size& size,
    const gfx::ColorSpace color_space,
    GrSurfaceOrigin surface_origin,
    SkAlphaType alpha_type,
    std::string debug_label,
    scoped_refptr<StreamTextureSharedImageInterface> stream_texture_sii,
    scoped_refptr<SharedContextState> context_state,
    scoped_refptr<RefCountedLock> drdc_lock)
    : OhosVideoImageBacking(mailbox,
                            size,
                            color_space,
                            surface_origin,
                            alpha_type,
                            std::move(debug_label),
                            !!drdc_lock),
      RefCountedLockHelperDrDc(std::move(drdc_lock)),
      stream_texture_sii_(std::move(stream_texture_sii)),
      gpu_main_task_runner_(base::SingleThreadTaskRunner::GetCurrentDefault()) {
  DCHECK(stream_texture_sii_);

  context_lost_helper_ = std::make_unique<ContextLostObserverHelper>(
      std::move(context_state), stream_texture_sii_, gpu_main_task_runner_,
      GetDrDcLock());
}

SameLayerNativeBufferImageBacking::~SameLayerNativeBufferImageBacking() {
  // This backing is created on gpu main thread but can be destroyed on DrDc
  // thread if the last representation was on DrDc thread.
  // |context_lost_helper_| is destroyed here by posting task to the
  // |gpu_main_thread_| to ensure that resources are cleaned up correvtly on
  // the gpu main thread.
  if (!gpu_main_task_runner_->RunsTasksInCurrentSequence()) {
    auto helper_destruction_cb = base::BindPostTask(
        gpu_main_task_runner_,
        base::BindOnce(
            [](std::unique_ptr<ContextLostObserverHelper> context_lost_helper,
               scoped_refptr<StreamTextureSharedImageInterface>
                   stream_texture_sii) {
              // Reset the |stream_texture_sii| first so that its ref in the
              // |context_lost_helper| gets reset under the DrDc lock.
              stream_texture_sii.reset();
              context_lost_helper.reset();
            }));
    std::move(helper_destruction_cb)
        .Run(std::move(context_lost_helper_), std::move(stream_texture_sii_));
  }
}
//LCOV_EXCL_STOP

// Representation of SameLayerNativeBufferImageBacking as a GL Texture.
class SameLayerNativeBufferImageBacking::GLTextureVideoImageRepresentation
    : public GLTextureImageRepresentation,
      public RefCountedLockHelperDrDc {
 public:
//LCOV_EXCL_START
  GLTextureVideoImageRepresentation(
      SharedImageManager* manager,
      SameLayerNativeBufferImageBacking* backing,
      MemoryTypeTracker* tracker,
      std::unique_ptr<AbstractTextureOHOS> texture,
      scoped_refptr<RefCountedLock> drdc_lock)
      : GLTextureImageRepresentation(manager, backing, tracker),
        RefCountedLockHelperDrDc(std::move(drdc_lock)),
        texture_(std::move(texture)) {}

  ~GLTextureVideoImageRepresentation() override {
    if (!has_context()) {
      texture_->NotifyOnContextLost();
    }
  }

  // Disallow copy and assign.
  GLTextureVideoImageRepresentation(const GLTextureVideoImageRepresentation&) =
      delete;
  GLTextureVideoImageRepresentation& operator=(
      const GLTextureVideoImageRepresentation&) = delete;

  gles2::Texture* GetTexture(int plane_index) override {
    DCHECK_EQ(plane_index, 0);

    auto* texture = gles2::Texture::CheckedCast(texture_->GetTextureBase());
    DCHECK(texture);

    return texture;
  }
//LCOV_EXCL_STOP

  bool BeginAccess(GLenum mode) override {
    TRACE_EVENT0("gpu",
                 "SameLayerNativeBufferImageBacking::"
                 "GLTextureVideoImageRepresentation::BeginAccess");
    // This representation should only be called for read.
    DCHECK(mode == GL_SHARED_IMAGE_ACCESS_MODE_READ_CHROMIUM);

    auto* native_buffer_backing =
        static_cast<SameLayerNativeBufferImageBacking*>(backing());
    {
      base::AutoLockMaybe auto_lock(GetDrDcLockPtr());
      scoped_native_buffer_ =
          native_buffer_backing->stream_texture_sii_->GetNativeBuffer();
    }
    if (!scoped_native_buffer_) {
      LOG(ERROR) << "Failed to get the hardware buffer.";
      return false;
    }

    gl::ohos::SyncFenceWait(scoped_native_buffer_->TakeFence());
    CreateAndBindEglImageFromNativeBuffer(scoped_native_buffer_->buffer(),
                                          texture_->service_id());
    return true;
  }

//LCOV_EXCL_START
  void EndAccess() override {
    DCHECK(scoped_native_buffer_);
    TRACE_EVENT0("gpu",
                 "SameLayerNativeBufferImageBacking::"
                 "GLTextureVideoImageRepresentation::EndAccess");
    base::ScopedFD sync_fd = gl::CreateEglFenceAndExportFd();

    scoped_native_buffer_->SetReadFence(std::move(sync_fd));
    base::AutoLockMaybe auto_lock(GetDrDcLockPtr());
    scoped_native_buffer_ = nullptr;
  }
//LCOV_EXCL_STOP

 private:
  std::unique_ptr<AbstractTextureOHOS> texture_;
  std::unique_ptr<ScopedNativeBufferFenceSync> scoped_native_buffer_;
};

class SameLayerNativeBufferImageBacking::SkiaVkSameLayerRepresentation
    : public SkiaVkNBImageRepresentation,
      public RefCountedLockHelperDrDc {
 public:
//LCOV_EXCL_START
  SkiaVkSameLayerRepresentation(SharedImageManager* manager,
                                OhosImageBacking* backing,
                                scoped_refptr<SharedContextState> context_state,
                                MemoryTypeTracker* tracker,
                                scoped_refptr<RefCountedLock> drdc_lock)
      : SkiaVkNBImageRepresentation(manager,
                                    backing,
                                    std::move(context_state),
                                    tracker),
        RefCountedLockHelperDrDc(std::move(drdc_lock)) {
    TRACE_EVENT2("base", __FILE__, "func", __func__, "line", __LINE__);
  }

  std::vector<sk_sp<SkSurface>> BeginWriteAccess(
      int final_msaa_count,
      const SkSurfaceProps& surface_props,
      const gfx::Rect& update_rect,
      std::vector<GrBackendSemaphore>* begin_semaphores,
      std::vector<GrBackendSemaphore>* end_semaphores,
      std::unique_ptr<skgpu::MutableTextureState>* end_state) override {
    // Writes are not intended to used for video backed representations.
    NOTIMPLEMENTED();
    return {};
  }

  void EndWriteAccess() override { NOTIMPLEMENTED(); }
//LCOV_EXCL_STOP

  std::vector<sk_sp<GrPromiseImageTexture>> BeginReadAccess(
      std::vector<GrBackendSemaphore>* begin_semaphores,
      std::vector<GrBackendSemaphore>* end_semaphores,
      std::unique_ptr<skgpu::MutableTextureState>* end_state) override {
    base::AutoLockMaybe auto_lock(GetDrDcLockPtr());

    TRACE_EVENT2("base", __FILE__, "func", __func__, "line", __LINE__);
    DCHECK(!scoped_native_buffer_);
    auto* samelayer_backing =
        static_cast<SameLayerNativeBufferImageBacking*>(backing());
    DCHECK(samelayer_backing);
    auto* stream_texture_sii = samelayer_backing->stream_texture_sii_.get();

    // GetAHardwareBuffer() renders the latest image and gets AHardwareBuffer
    // from it.
    scoped_native_buffer_ = stream_texture_sii->GetNativeBuffer();
    if (!scoped_native_buffer_) {
      LOG(ERROR) << "Failed to get the hardware buffer.";
      return {};
    }
    DCHECK(scoped_native_buffer_->buffer());

    // Wait on the sync fd attached to the buffer to make sure buffer is
    // ready before the read. This is done by inserting the sync fd semaphore
    // into begin_semaphore vector which client will wait on.
    init_read_fence_ = scoped_native_buffer_->TakeFence();
    std::shared_ptr<OHOS::NWeb::NativeBufferConfigAdapterImpl> 
        configAdapterTmp =
            std::make_shared<OHOS::NWeb::NativeBufferConfigAdapterImpl>();
    if (configAdapterTmp) {
      OHOS::NWeb::OhosAdapterHelper::GetInstance()
        .GetOhosNativeBufferAdapter()
        .Describe(configAdapterTmp, scoped_native_buffer_->buffer());
    } else {
      return {};
    }
    
    if (!vulkan_image_) {
      DCHECK(!promise_texture_);
      real_size_ = size();
      real_size_.set_width(std::min(size().width(), configAdapterTmp->GetBufferWidth()));
      real_size_.set_height(std::min(size().height(), configAdapterTmp->GetBufferHeight()));
      if (real_size_ != size()) {
        LOG(INFO) << "SameLayerNativeBufferImageBacking create vkimage width: "
          << real_size_.width() << " height: " << real_size_.height()
          << " backing width: " << size().width() << " height: " << size().height();
      }
      vulkan_image_ = CreateVkImageFromNativeBufferHandle(
          scoped_native_buffer_->TakeBuffer(), context_state(), real_size_,
          format(), VK_QUEUE_FAMILY_FOREIGN_EXT);
      if (!vulkan_image_) {
        return {};
      }

      // We always use VK_IMAGE_TILING_OPTIMAL while creating the vk image in
      // VulkanImplementationAndroid::CreateVkImageAndImportAHB. Hence pass
      // the tiling parameter as VK_IMAGE_TILING_OPTIMAL to below call rather
      // than passing |vk_image_info.tiling|. This is also to ensure that the
      // promise image created here at [1] as well the fulfill image created
      // via the current function call are consistent and both are using
      // VK_IMAGE_TILING_OPTIMAL. [1] -
      // https://cs.chromium.org/chromium/src/components/viz/service/display_embedder/skia_output_surface_impl.cc?rcl=db5ffd448ba5d66d9d3c5c099754e5067c752465&l=789.
      DCHECK_EQ(static_cast<int32_t>(vulkan_image_->image_tiling()),
                static_cast<int32_t>(VK_IMAGE_TILING_OPTIMAL));

      // TODO(bsalomon): Determine whether it makes sense to attempt to reuse
      // this if the vk_info stays the same on subsequent calls.
      promise_texture_ = GrPromiseImageTexture::Make(GrBackendTextures::MakeVk(
          size().width(), size().height(),
          CreateGrVkImageInfo(vulkan_image_.get(), format(), color_space())));
      DCHECK(promise_texture_);
    }

    return SkiaVkNBImageRepresentation::BeginReadAccess(
        begin_semaphores, end_semaphores, end_state);
  }

//LCOV_EXCL_START
  void EndReadAccess() override {
    base::AutoLockMaybe auto_lock(GetDrDcLockPtr());
    DCHECK(scoped_native_buffer_);

    TRACE_EVENT2("base", __FILE__, "func", __func__, "line", __LINE__);
    SkiaVkNBImageRepresentation::EndReadAccess();

    // Pass the end read access sync fd to the scoped hardware buffer. This
    // will make sure that the AImage associated with the hardware buffer will
    // be deleted only when the read access is ending.
    scoped_native_buffer_->SetReadFence(ohos_backing()->TakeReadFence());
    scoped_native_buffer_ = nullptr;
  }
//LCOV_EXCL_STOP

 private:
  gfx::Size real_size_;
  std::unique_ptr<ScopedNativeBufferFenceSync> scoped_native_buffer_;
};

//LCOV_EXCL_START
std::unique_ptr<GLTextureImageRepresentation>
SameLayerNativeBufferImageBacking::ProduceGLTexture(
    SharedImageManager* manager,
    MemoryTypeTracker* tracker) {
  base::AutoLockMaybe auto_lock(GetDrDcLockPtr());

  // For (old) overlays, we don't have a texture owner, but overlay promotion
  // might not happen for some reasons. In that case, it will try to draw
  // which should result in no image.
  if (!stream_texture_sii_->HasTextureOwner()) {
    return nullptr;
  }

  // Generate an abstract texture.
  auto texture = GenAbstractTexture(/*passthrough=*/false);
  if (!texture) {
    return nullptr;
  }

  return std::make_unique<GLTextureVideoImageRepresentation>(
      manager, this, tracker, std::move(texture), GetDrDcLock());
}

std::unique_ptr<SkiaGaneshImageRepresentation>
SameLayerNativeBufferImageBacking::ProduceSkiaGanesh(
    SharedImageManager* manager,
    MemoryTypeTracker* tracker,
    scoped_refptr<SharedContextState> context_state) {
  TRACE_EVENT0("gpu", "SameLayerNativeBufferImageBacking::ProduceSkiaGanesh");
  base::AutoLockMaybe auto_lock(GetDrDcLockPtr());

  DCHECK(context_state);

  // For (old) overlays, we don't have a texture owner, but overlay promotion
  // might not happen for some reasons. In that case, it will try to draw
  // which should result in no image.
  if (!stream_texture_sii_->HasTextureOwner()) {
    return nullptr;
  }

  DCHECK(context_state->GrContextIsGL());

  auto texture = GenAbstractTexture(false);
  if (!texture) {
    return nullptr;
  }

  if (context_state->GrContextIsVulkan()) {
    return std::make_unique<SkiaVkSameLayerRepresentation>(
        manager, this, std::move(context_state), tracker, GetDrDcLock());
  }

  std::unique_ptr<GLTextureImageRepresentationBase> gl_representation;
  gl_representation = std::make_unique<GLTextureVideoImageRepresentation>(
      manager, this, tracker, std::move(texture), GetDrDcLock());

  return SkiaGLImageRepresentation::Create(std::move(gl_representation),
                                           std::move(context_state), manager,
                                           this, tracker);
}

SameLayerNativeBufferImageBacking::ContextLostObserverHelper::
    ContextLostObserverHelper(
        scoped_refptr<SharedContextState> context_state,
        scoped_refptr<StreamTextureSharedImageInterface> stream_texture_sii,
        scoped_refptr<base::SingleThreadTaskRunner> gpu_main_task_runner,
        scoped_refptr<RefCountedLock> drdc_lock)
    : RefCountedLockHelperDrDc(std::move(drdc_lock)),
      context_state_(std::move(context_state)),
      stream_texture_sii_(std::move(stream_texture_sii)),
      gpu_main_task_runner_(std::move(gpu_main_task_runner)) {
  DCHECK(context_state_);
  DCHECK(stream_texture_sii_);

  context_state_->AddContextLostObserver(this);
}

SameLayerNativeBufferImageBacking::ContextLostObserverHelper::
    ~ContextLostObserverHelper() {
  DCHECK(gpu_main_task_runner_->RunsTasksInCurrentSequence());

  if (context_state_) {
    context_state_->RemoveContextLostObserver(this);
  }
  {
    base::AutoLockMaybe auto_lock(GetDrDcLockPtr());
    stream_texture_sii_->ReleaseResources();
    stream_texture_sii_.reset();
  }
}

// SharedContextState::ContextLostObserver implementation.
void SameLayerNativeBufferImageBacking::ContextLostObserverHelper::
    OnContextLost() {
  DCHECK(gpu_main_task_runner_->RunsTasksInCurrentSequence());
  base::AutoLockMaybe auto_lock(GetDrDcLockPtr());

  // We release codec buffers when shared image context is lost. This is
  // because texture owner's texture was created on shared context. Once
  // shared context is lost, no one should try to use that texture.
  stream_texture_sii_->ReleaseResources();
  context_state_->RemoveContextLostObserver(this);
  context_state_ = nullptr;
}
//LCOV_EXCL_STOP
}  // namespace gpu
