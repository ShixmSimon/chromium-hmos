// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Based on stream_texture_android.cc originally written by
// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. 

#include "gpu/ipc/service/stream_texture_ohos.h"

#include <string.h>

#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "gpu/command_buffer/service/context_state.h"
// #include "gpu/command_buffer/service/mailbox_manager.h"
#include "gpu/command_buffer/service/ohos/ohos_video_image_backing.h"
#include "gpu/command_buffer/service/scheduler.h"
#include "gpu/command_buffer/service/scheduler_task_runner.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#include "gpu/command_buffer/service/shared_image/shared_image_backing.h"
#include "gpu/command_buffer/service/shared_image/shared_image_factory.h"
#include "gpu/config/gpu_finch_features.h"
#include "gpu/ipc/common/command_buffer_id.h"
#include "gpu/ipc/common/gpu_channel.mojom.h"
#include "gpu/ipc/common/gpu_surface_id_tracker.h"
#include "gpu/ipc/service/gpu_channel.h"
#include "gpu/ipc/service/gpu_channel_manager.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gl/gl_context.h"
#include "ui/gl/scoped_binders.h"
#include "ui/gl/scoped_make_current.h"

namespace gpu {
namespace {

std::unique_ptr<ui::ScopedMakeCurrent> MakeCurrent(
    SharedContextState* context_state) {
  std::unique_ptr<ui::ScopedMakeCurrent> scoped_make_current;
  bool needs_make_current =
      !context_state->IsCurrent(nullptr, /*needs_gl=*/true);
  if (needs_make_current) {
    scoped_make_current = std::make_unique<ui::ScopedMakeCurrent>(
        context_state->context(), context_state->surface());
  }
  return scoped_make_current;
}

//LCOV_EXCL_START
scoped_refptr<gpu::RefCountedLock> CreateDrDcLockIfNeeded() {
  return base::MakeRefCounted<gpu::RefCountedLock>();
}
//LCOV_EXCL_STOP

}  // namespace

// static
scoped_refptr<StreamTexture> StreamTexture::Create(
    GpuChannel* channel,
    int stream_id,
    gl::ohos::TextureOwnerMode texture_owner_mode,
    mojo::PendingAssociatedReceiver<mojom::StreamTexture> receiver) {
  LOG(INFO) << "[NativeEmbed] StreamTexture::Create.";
  ContextResult result;
  auto context_state =
      channel->gpu_channel_manager()->GetSharedContextState(&result);
  if (result != ContextResult::kSuccess) {
    return nullptr;
  }
  auto scoped_make_current = MakeCurrent(context_state.get());
  if (scoped_make_current && !scoped_make_current->IsContextCurrent()) {
    return nullptr;
  }
  return new StreamTexture(channel, stream_id, texture_owner_mode,
                           std::move(receiver), std::move(context_state));
}

// static
void StreamTexture::RunCallback(
    scoped_refptr<base::SingleThreadTaskRunner> task_runner,
    base::WeakPtr<StreamTexture> weak_stream_texture) {
  if (task_runner->BelongsToCurrentThread()) {
    if (weak_stream_texture) {
      weak_stream_texture->OnFrameAvailable();
    }
  } else {
    task_runner->PostTask(
        FROM_HERE, base::BindOnce(&StreamTexture::RunCallback, task_runner,
                                  std::move(weak_stream_texture)));
  }
}

StreamTexture::StreamTexture(
    GpuChannel* channel,
    int32_t route_id,
    gl::ohos::TextureOwnerMode texture_owner_mode,
    mojo::PendingAssociatedReceiver<mojom::StreamTexture> receiver,
    scoped_refptr<SharedContextState> context_state)
    : RefCountedLockHelperDrDc(CreateDrDcLockIfNeeded()),
      native_texture_owner_(NativeImageTextureOwner::Create(context_state,
                                                            texture_owner_mode,
                                                            GetDrDcLock())),
      has_pending_frame_(false),
      texture_owner_mode_(texture_owner_mode),
      channel_(channel),
      route_id_(route_id),
      context_state_(std::move(context_state)),
      sequence_(channel_->scheduler()->CreateSequence(SchedulingPriority::kLow,
                                                      channel_->task_runner())),
      receiver_(
          this,
          std::move(receiver),
          base::MakeRefCounted<SchedulerTaskRunner>(*channel_->scheduler(),
                                                    sequence_)) {
  channel_->AddRoute(route_id, sequence_);
  native_texture_owner_->SetFrameAvailableCallback(
      base::BindRepeating(&StreamTexture::RunCallback,
                          base::SingleThreadTaskRunner::GetCurrentDefault(),
                          weak_factory_.GetWeakPtr()));
}

//LCOV_EXCL_START
StreamTexture::~StreamTexture() {
  DCHECK_CALLED_ON_VALID_THREAD(gpu_main_thread_checker_);

  // |channel_| is always released before GpuChannel releases its reference to
  // this class.
  DCHECK(!channel_);

  gpu::GpuSurfaceIdTracker::Get()->RemoveSurface(native_embed_id_);
  native_embed_id_ = -1;
}

void StreamTexture::ReleaseChannel() {
  DCHECK_CALLED_ON_VALID_THREAD(gpu_main_thread_checker_);
  DCHECK(channel_);
  receiver_.ResetFromAnotherSequenceUnsafe();
  channel_->RemoveRoute(route_id_);
  channel_->scheduler()->DestroySequence(sequence_);
  sequence_ = SequenceId();
  channel_ = nullptr;
}

bool StreamTexture::IsUsingGpuMemory() const {
  // Once the image is bound during the first update, we just replace/update the
  // same image every time in future and hence the image is always bound to a
  // texture. This means that it always uses gpu memory.
  return true;
}

void StreamTexture::UpdateAndBindTexImage(GLuint service_id) {
  // UpdateTexImage happens via OnFrameAvailable callback now. So we
  // just need to ensure that image is bound to the correct texture id.
  DCHECK_GT(service_id, static_cast<unsigned>(0));
  native_texture_owner_->EnsureNativeImageBound(service_id);
}

bool StreamTexture::HasTextureOwner() const {
  return !!native_texture_owner_;
}

TextureBase* StreamTexture::GetTextureBase() const {
  return native_texture_owner_->GetTextureBase();
}

void StreamTexture::NotifyOverlayPromotion(bool promotion,
                                           const gfx::Rect& bounds) {}

bool StreamTexture::RenderToOverlay() {
  NOTREACHED();
}

bool StreamTexture::TextureOwnerBindsTextureOnUpdate() {
  DCHECK(native_texture_owner_);
  return native_texture_owner_->binds_texture_on_update();
}

void StreamTexture::OnFrameAvailable() {
  DCHECK_CALLED_ON_VALID_THREAD(gpu_main_thread_checker_);
  has_pending_frame_ = true;

  if (!client_ || !native_texture_owner_ || !channel_) {
    return;
  }

  // We haven't received size for first time yet from the MediaPlayer we will
  // defer this sending OnFrameAvailable till then.
  if (rotated_visible_size_.IsEmpty()) {
    return;
  }

  TRACE_EVENT2("base", __FILE__, "func", __func__, "line", __LINE__);
  native_texture_owner_->UpdateNativeImage();
  has_pending_frame_ = false;

  gfx::Rect visible_rect;
  gfx::Size coded_size;
  if (!native_texture_owner_->GetCodedSizeAndVisibleRect(
          rotated_visible_size_, &coded_size, &visible_rect)) {
    // if we failed to get right size fallback to visible size.
    coded_size = rotated_visible_size_;
    visible_rect = gfx::Rect(coded_size);
  }

  if (coded_size != coded_size_ || visible_rect != visible_rect_) {
    coded_size_ = coded_size;
    visible_rect_ = visible_rect;

    auto mailbox = CreateSharedImage(coded_size);
    gpu::VulkanContextProvider* vulkan_context_provider = nullptr;
    if (context_state_->GrContextIsVulkan()) {
      vulkan_context_provider = context_state_->vk_context_provider();
    }

    client_->OnFrameWithInfoAvailable(mailbox, coded_size, visible_rect,
                                      absl::nullopt);
  } else {
    client_->OnFrameAvailable();
  }
}

void StreamTexture::StartListening(
    mojo::PendingAssociatedRemote<mojom::StreamTextureClient> client) {
  client_.Bind(std::move(client));
}
//LCOV_EXCL_STOP

gpu::Mailbox StreamTexture::CreateSharedImage(const gfx::Size& coded_size) {
  DCHECK_CALLED_ON_VALID_THREAD(gpu_main_thread_checker_);
  // We do not update |texture_owner_texture_|'s internal gles2::Texture's
  // size. This is because the gles2::Texture is never used directly, the
  // associated |texture_owner_texture_id_| being the only part of that
  // object we interact with. If we ever use |texture_owner_texture_|, we
  // need to ensure that it gets updated here.

  auto scoped_make_current = MakeCurrent(context_state_.get());
  auto mailbox = gpu::Mailbox::Generate();
  auto shared_image = OhosVideoImageBacking::Create(
      mailbox, coded_size, gfx::ColorSpace::CreateSRGB(),
      kTopLeft_GrSurfaceOrigin, kPremul_SkAlphaType,
      /*debug_label=*/"StreamTexture", texture_owner_mode_, this,
      context_state_, GetDrDcLock());
  channel_->shared_image_stub()->factory()->RegisterBacking(
      std::move(shared_image));

  return mailbox;
}

void StreamTexture::UpdateRotatedVisibleSize(
    const gfx::Size& rotated_visible_size) {
  DCHECK_CALLED_ON_VALID_THREAD(gpu_main_thread_checker_);
  DCHECK(channel_);
  bool was_empty = rotated_visible_size_.IsEmpty();
  rotated_visible_size_ = rotated_visible_size;

  // It's possible that first OnUpdateRotatedVisibleSize will come after first
  // OnFrameAvailable. We delay sending OnFrameWithInfoAvailable if it comes
  // first so now it's time to send it.
  if (was_empty && has_pending_frame_) {
    OnFrameAvailable();
  }
}

//LCOV_EXCL_START
std::unique_ptr<ScopedNativeBufferFenceSync> StreamTexture::GetNativeBuffer() {
  DCHECK(native_texture_owner_);

  return native_texture_owner_->GetNativeBuffer();
}

int StreamTexture::NativeEmbedID() {
  if (native_embed_id_ == -1) {
    uint64_t id;
    native_texture_owner_->GetSurfaceId(&id);
    std::string native_surface_id = std::to_string(id);
    native_embed_id_ =
        gpu::GpuSurfaceIdTracker::Get()->AddSurfaceForNativeWidget(
            gpu::GpuSurfaceIdTracker::SurfaceRecord(gfx::kNullAcceleratedWidget,
                                                    native_surface_id));
  }
  return native_embed_id_;
}
//LCOV_EXCL_STOP

}  // namespace gpu
                   