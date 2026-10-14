// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gpu/ohos/ohos_video_frame_converter.h"

#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/bind_post_task.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "gpu/command_buffer/common/shared_image_info.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/command_buffer/common/sync_token.h"
#include "gpu/command_buffer/service/scheduler.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#include "gpu/ipc/service/gpu_channel.h"
#include "gpu/ipc/service/gpu_channel_shared_image_interface.h"
#include "gpu/ipc/service/shared_image_stub.h"
#include "gpu/vulkan/buildflags.h"
#include "media/base/format_utils.h"
#include "media/base/video_types.h"
#include "ui/gfx/gpu_fence.h"
#include "ui/gl/gl_bindings.h"
#include "ui/gl/gl_fence.h"
#include "ui/gl/gl_surface_egl.h"
#include "ui/ozone/platform/ohos/ohos_native_pixmap.h"

#if BUILDFLAG(ENABLE_VULKAN)
#include "components/viz/common/gpu/vulkan_context_provider.h"
#include "ui/ozone/platform/ohos/ohos_video_vulkan.h"
#endif

namespace media {
namespace {

constexpr base::TimeDelta kIdleImportCacheTimeout = base::Seconds(10);

}  // namespace

OhosVideoFrameConverter::OhosVideoFrameConverter(
    scoped_refptr<base::SequencedTaskRunner> gpu_task_runner,
    GetCommandBufferStubCB get_stub_cb,
    const gpu::GpuDriverBugWorkarounds& workarounds)
    : base::RefCountedDeleteOnSequence<OhosVideoFrameConverter>(
          gpu_task_runner),
      gpu_task_runner_(std::move(gpu_task_runner)),
      get_stub_cb_(std::move(get_stub_cb)),
      workarounds_(workarounds) {}

OhosVideoFrameConverter::~OhosVideoFrameConverter() {
  DCHECK(gpu_task_runner_->RunsTasksInCurrentSequence());
  DestroyStub();
}

void OhosVideoFrameConverter::Initialize(viz::SharedImageFormat format,
                                         bool is_encrypted,
                                         base::OnceCallback<void(bool)> done) {
  DCHECK(gpu_task_runner_->RunsTasksInCurrentSequence());
  if (!initialized_) {
    initialized_ = true;
    stub_ = get_stub_cb_ ? get_stub_cb_.Run() : nullptr;
    get_stub_cb_.Reset();
    if (stub_) {
      stub_->AddDestructionObserver(this);
      wait_sequence_id_ = stub_->channel()->scheduler()->CreateSequence(
          gpu::SchedulingPriority::kHigh, stub_->channel()->task_runner());
      sis_ = stub_->channel()->shared_image_stub();
    }
  }
  use_vulkan_ = sis_ && sis_->shared_context_state()->GrContextIsVulkan();
  bool supported = sis_ && sis_->MakeContextCurrent(/*needs_gl=*/!use_vulkan_) &&
                   !sis_->shared_context_state()->context_lost() &&
                   ui::IsOhosNativePixmapFormat(format);
  if (use_vulkan_) {
#if BUILDFLAG(ENABLE_VULKAN)
    // Protected imports need a protected VkDevice/queue, which this path
    // does not create. Keep encrypted playback on the existing fallback.
    auto context = sis_->shared_context_state();
    supported = supported && !is_encrypted &&
                context->vk_context_provider() &&
                context->external_semaphore_pool() &&
                ui::CanImportOhosVideoVulkan(
                    context->vk_context_provider()->GetDeviceQueue());
#else
    supported = false;
#endif
    LOG(WARNING) << "OHOS video zero-copy: native Vulkan available=" << supported;
    std::move(done).Run(supported);
    return;
  }
  // ANGLE Vulkan still uses the GL/EGL import path.
  supported = supported && sis_->shared_context_state()->GrContextIsGL() &&
              gl::GLFence::IsGpuFenceSupported();
  if (supported) {
    auto* display = gl::GLSurfaceEGL::GetGLDisplayEGL();
    const char* extensions =
        display ? eglQueryString(display->GetDisplay(), EGL_EXTENSIONS)
                : nullptr;
    supported = extensions &&
                std::string(extensions).find("EGL_OHOS_image_native_buffer") !=
                    std::string::npos;
  }
  std::move(done).Run(supported);
}

void OhosVideoFrameConverter::OnWillDestroyStub(bool have_context) {
  // Cancelling the wait sequence can drop the last frame callback's reference.
  auto keep_alive = base::WrapRefCounted(this);
  DestroyStub();
}

void OhosVideoFrameConverter::DestroyStub() {
  DCHECK(gpu_task_runner_->RunsTasksInCurrentSequence());
  idle_cache_timer_.Stop();
  LogCacheStats();
  image_cache_.Clear();
  sis_ = nullptr;
  if (stub_) {
    stub_->channel()->scheduler()->DestroySequence(wait_sequence_id_);
    stub_->RemoveDestructionObserver(this);
    stub_ = nullptr;
  }
}

void OhosVideoFrameConverter::Reset(uint32_t generation) {
  DCHECK(gpu_task_runner_->RunsTasksInCurrentSequence());
  idle_cache_timer_.Stop();
  LogCacheStats();
  image_cache_.Reset(generation);
  images_created_ = 0;
  images_reused_ = 0;
  uncached_frames_ = 0;
}

void OhosVideoFrameConverter::Convert(uint32_t generation,
                                      scoped_refptr<gfx::NativePixmap> pixmap,
                                      gfx::GpuFenceHandle acquire_fence,
                                      const gfx::Rect& visible_rect,
                                      const gfx::Size& natural_size,
                                      const gfx::ColorSpace& color_space,
                                      const gfx::HDRMetadata& hdr_metadata,
                                      base::TimeDelta timestamp,
                                      OutputCB output_cb) {
  DCHECK(gpu_task_runner_->RunsTasksInCurrentSequence());
  // Keep each failure distinguishable in normal hilog; device diagnostics
  // must not require an extra feature or verbose logging switch.
  auto fail = [&](const char* reason) {
    LOG(ERROR) << "OHOS video zero-copy: conversion failed: " << reason
               << " generation=" << generation
               << " cache_generation=" << image_cache_.generation()
               << " visible=" << visible_rect.ToString()
               << " natural=" << natural_size.ToString();
    std::move(output_cb).Run(nullptr);
  };
  if (!sis_) {
    fail("SharedImage stub unavailable");
    return;
  }
  if (generation != image_cache_.generation()) {
    fail("cache generation mismatch before import");
    return;
  }
  if (!pixmap) {
    fail("native pixmap missing");
    return;
  }
  if (!gfx::Rect(pixmap->GetBufferSize()).Contains(visible_rect)) {
    LOG(ERROR) << "OHOS video zero-copy: allocation="
               << pixmap->GetBufferSize().ToString();
    fail("visible rect outside native allocation");
    return;
  }
  if (!sis_->MakeContextCurrent(/*needs_gl=*/!use_vulkan_)) {
    fail("MakeContextCurrent failed");
    return;
  }
  if (sis_->shared_context_state()->context_lost()) {
    fail("GPU context lost");
    return;
  }
  // External sampling is a SharedImage detail. Select the VideoFrame format
  // from the actual native allocation, not the codec profile or a fixed NV12.
  auto format = pixmap->GetSharedImageFormat();
  format.ClearPrefersExternalSampler();
  auto pixel_format = SharedImageFormatToVideoPixelFormat(format);
  if (!pixel_format || (*pixel_format != PIXEL_FORMAT_NV12 &&
                        *pixel_format != PIXEL_FORMAT_P010LE)) {
    LOG(ERROR) << "OHOS video zero-copy: native SharedImage format="
               << format.ToString();
    fail("unsupported VideoFrame pixel format");
    return;
  }
  // Read-only GPU uses. No scanout, CPU mapping, or direct WebGPU import.
  gpu::SharedImageUsageSet usage = gpu::SHARED_IMAGE_USAGE_DISPLAY_READ |
                                   gpu::SHARED_IMAGE_USAGE_RASTER_READ;
  if (!use_vulkan_) {
    usage |= gpu::SHARED_IMAGE_USAGE_GLES2_READ;
  }
  const gpu::SharedImageInfo info(pixmap->GetSharedImageFormat(),
                                  pixmap->GetBufferSize(), color_space,
                                  kTopLeft_GrSurfaceOrigin, kOpaque_SkAlphaType,
                                  usage,
                                  "OhosVideoDecoder");
  // Vulkan retains the original Surface lease in VkImage. Caching that image
  // would pin decoder queue slots; use one import per frame until a separate
  // cache/lease protocol is implemented for Vulkan.
  std::optional<uint32_t> reusable_buffer_id =
      use_vulkan_ ? std::nullopt : ui::GetOhosVideoNativePixmapId(*pixmap);
  scoped_refptr<gpu::ClientSharedImage> shared_image;
  gpu::SyncToken ready_token;
  const bool had_acquire_fence = !acquire_fence.is_null();
  if (reusable_buffer_id) {
    shared_image = image_cache_.Take(*reusable_buffer_id,
                                    pixmap->GetSharedImageFormat(),
                                    pixmap->GetBufferSize(), color_space);
  }
  const bool reused = !!shared_image;
  if (shared_image) {
    std::unique_ptr<gfx::GpuFence> fence;
    if (!acquire_fence.is_null()) {
      fence = std::make_unique<gfx::GpuFence>(std::move(acquire_fence));
    }
    // A reused mailbox now contains new pixels. The update installs this
    // decode's producer fence; its new token orders all subsequent readers.
    ready_token = shared_image->BackingWasExternallyUpdated(std::move(fence));
    sis_->shared_image_interface()->VerifySyncToken(ready_token);
    if (++images_reused_ == 1) {
      LOG(WARNING) << "OHOS video zero-copy: SharedImage reuse active";
    }
  } else {
    scoped_refptr<gfx::NativePixmap> import_pixmap;
    if (reusable_buffer_id) {
      import_pixmap = ui::CloneOhosVideoNativePixmapForImport(*pixmap);
    }
    if (!import_pixmap) {
      // An uncached frame retains the original ownership/fence protocol.
      // Never cache that pixmap: it would hold a consumer queue slot forever.
      reusable_buffer_id.reset();
      import_pixmap = pixmap;
      if (uncached_frames_ == 0) {
        LOG(WARNING) << "OHOS video zero-copy: uncached frame import; Vulkan="
                     << use_vulkan_;
      }
      ++uncached_frames_;
    }
    shared_image =
        sis_->shared_image_interface()->CreateSharedImageForOhosVideo(
            info, std::move(import_pixmap), std::move(acquire_fence),
            workarounds_);
    if (shared_image) {
      ready_token = shared_image->creation_sync_token();
      ++images_created_;
    }
  }
  if (!shared_image) {
    LOG(ERROR) << "OHOS video zero-copy: import format="
               << pixmap->GetSharedImageFormat().ToString()
               << " allocation=" << pixmap->GetBufferSize().ToString()
               << " cacheable=" << reusable_buffer_id.has_value()
               << " acquire_fence=" << had_acquire_fence;
    fail("SharedImage creation/import failed");
    return;
  }
  auto release_cb = base::BindPostTask(
      gpu_task_runner_,
      base::BindOnce(&OhosVideoFrameConverter::OnVideoFrameReleased,
                     base::WrapRefCounted(this), generation, reusable_buffer_id,
                     shared_image, std::move(pixmap), ready_token));
  auto frame = VideoFrame::WrapSharedImage(
      *pixel_format, shared_image, ready_token,
      std::move(release_cb), visible_rect, natural_size, timestamp);
  if (!frame) {
    LOG(ERROR) << "OHOS video zero-copy: frame wrap format="
               << VideoPixelFormatToString(*pixel_format)
               << " allocation=" << shared_image->size().ToString()
               << " reused=" << reused
               << " acquire_fence=" << had_acquire_fence
               << " ready_token=" << ready_token.HasData()
               << " verified=" << ready_token.verified_flush();
    fail("VideoFrame::WrapSharedImage failed");
    return;
  }
  frame->set_color_space(color_space);
  if (color_space.IsHDR()) {
    frame->set_hdr_metadata(hdr_metadata);
  }
  frame->metadata().read_lock_fences_enabled = true;
  frame->metadata().power_efficient = true;
  if ((images_created_ + images_reused_) % 300 == 0) {
    LogCacheStats();
  }
  std::move(output_cb).Run(std::move(frame));
}

void OhosVideoFrameConverter::OnVideoFrameReleased(
    uint32_t generation,
    std::optional<uint32_t> reusable_buffer_id,
    scoped_refptr<gpu::ClientSharedImage> shared_image,
    scoped_refptr<gfx::NativePixmap> pixmap,
    gpu::SyncToken ready_token,
    const gpu::SyncToken& sync_token) {
  DCHECK(gpu_task_runner_->RunsTasksInCurrentSequence());
  shared_image->UpdateDestructionSyncToken(sync_token);
  if (!stub_) {
    return;
  }
  // A mailbox release means submitted work, not necessarily completed work.
  // Hold the native buffer until Chromium's read-lock completion token passes.
  // Also wait for the external update if a frame was dropped without readers.
  // Keep the converter alive until the wait completes: decoder destruction
  // alone must not destroy the sequence and release an in-flight frame early.
  stub_->channel()->scheduler()->ScheduleTask(gpu::Scheduler::Task(
      wait_sequence_id_,
      base::BindOnce(&OhosVideoFrameConverter::OnVideoFrameReadComplete,
                     base::WrapRefCounted(this), generation, reusable_buffer_id,
                     std::move(shared_image), std::move(pixmap)),
      std::vector<gpu::SyncToken>{sync_token, ready_token}));
}

void OhosVideoFrameConverter::OnVideoFrameReadComplete(
    uint32_t generation,
    std::optional<uint32_t> reusable_buffer_id,
    scoped_refptr<gpu::ClientSharedImage> shared_image,
    scoped_refptr<gfx::NativePixmap> pixmap) {
  DCHECK(gpu_task_runner_->RunsTasksInCurrentSequence());
  if (sis_ && reusable_buffer_id && generation == image_cache_.generation()) {
    image_cache_.Put(generation, *reusable_buffer_id, std::move(shared_image));
    idle_cache_timer_.Start(
        FROM_HERE, kIdleImportCacheTimeout, this,
        &OhosVideoFrameConverter::ClearIdleCache);
  }
  // Only the per-frame pixmap owns the queue lease. Dropping it posts the
  // release to the decoder even while the cached import remains alive.
}

void OhosVideoFrameConverter::ClearIdleCache() {
  DCHECK(gpu_task_runner_->RunsTasksInCurrentSequence());
  LogCacheStats();
  image_cache_.Clear();
}

void OhosVideoFrameConverter::LogCacheStats() {
  if (images_created_ || images_reused_) {
    VLOG(1) << "OHOS video SharedImage cache: generation="
            << image_cache_.generation() << " created=" << images_created_
            << " reused=" << images_reused_ << " uncached=" << uncached_frames_
            << " idle=" << image_cache_.size()
            << " estimated_bytes=" << image_cache_.bytes();
  }
}

}  // namespace media
