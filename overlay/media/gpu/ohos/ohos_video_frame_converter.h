// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_GPU_OHOS_OHOS_VIDEO_FRAME_CONVERTER_H_
#define MEDIA_GPU_OHOS_OHOS_VIDEO_FRAME_CONVERTER_H_

#include <optional>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted_delete_on_sequence.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "gpu/command_buffer/service/sequence_id.h"
#include "gpu/config/gpu_driver_bug_workarounds.h"
#include "gpu/ipc/service/command_buffer_stub.h"
#include "media/base/video_frame.h"
#include "media/gpu/ohos/ohos_video_shared_image_cache.h"
#include "ui/gfx/gpu_fence_handle.h"
#include "ui/gfx/native_pixmap.h"

namespace gpu {
class ClientSharedImage;
class SharedImageStub;
struct SyncToken;
}  // namespace gpu

namespace media {

// GPU-sequence bridge for decoder-owned buffers. GL imports can be cached;
// Vulkan imports keep the queue lease until VkImage fence-helper cleanup.
class OhosVideoFrameConverter
    : public gpu::CommandBufferStub::DestructionObserver,
      public base::RefCountedDeleteOnSequence<OhosVideoFrameConverter> {
 public:
  using GetCommandBufferStubCB =
      base::RepeatingCallback<gpu::CommandBufferStub*()>;
  using OutputCB = base::OnceCallback<void(scoped_refptr<VideoFrame>)>;

  OhosVideoFrameConverter(
      scoped_refptr<base::SequencedTaskRunner> gpu_task_runner,
      GetCommandBufferStubCB get_stub_cb,
      const gpu::GpuDriverBugWorkarounds& workarounds);

  // All public methods run on the GPU sequence; construction may run elsewhere.
  void Initialize(viz::SharedImageFormat format,
                  bool is_encrypted,
                  base::OnceCallback<void(bool)> done);
  void Reset(uint32_t generation);
  void Convert(uint32_t generation,
               scoped_refptr<gfx::NativePixmap> pixmap,
               gfx::GpuFenceHandle acquire_fence,
               const gfx::Rect& visible_rect,
               const gfx::Size& natural_size,
               const gfx::ColorSpace& color_space,
               const gfx::HDRMetadata& hdr_metadata,
               base::TimeDelta timestamp,
               OutputCB output_cb);

 private:
  friend class base::DeleteHelper<OhosVideoFrameConverter>;
  friend class base::RefCountedDeleteOnSequence<OhosVideoFrameConverter>;
  ~OhosVideoFrameConverter() override;
  void OnWillDestroyStub(bool have_context) override;
  void DestroyStub();
  void OnVideoFrameReleased(uint32_t generation,
                            std::optional<uint32_t> reusable_buffer_id,
                            scoped_refptr<gpu::ClientSharedImage> shared_image,
                            scoped_refptr<gfx::NativePixmap> pixmap,
                            gpu::SyncToken ready_token,
                            const gpu::SyncToken& sync_token);
  void OnVideoFrameReadComplete(
      uint32_t generation,
      std::optional<uint32_t> reusable_buffer_id,
      scoped_refptr<gpu::ClientSharedImage> shared_image,
      scoped_refptr<gfx::NativePixmap> pixmap);
  void ClearIdleCache();
  void LogCacheStats();

  const scoped_refptr<base::SequencedTaskRunner> gpu_task_runner_;
  GetCommandBufferStubCB get_stub_cb_;
  const gpu::GpuDriverBugWorkarounds workarounds_;
  bool initialized_ = false;
  bool use_vulkan_ = false;
  bool use_graphite_ = false;
  raw_ptr<gpu::CommandBufferStub> stub_ = nullptr;
  raw_ptr<gpu::SharedImageStub> sis_ = nullptr;
  gpu::SequenceId wait_sequence_id_;
  OhosVideoSharedImageCache image_cache_;
  base::OneShotTimer idle_cache_timer_;
  uint64_t images_created_ = 0;
  uint64_t images_reused_ = 0;
  uint64_t uncached_frames_ = 0;
};

}  // namespace media
#endif  // MEDIA_GPU_OHOS_OHOS_VIDEO_FRAME_CONVERTER_H_
