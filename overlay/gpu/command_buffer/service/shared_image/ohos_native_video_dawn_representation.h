// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GPU_COMMAND_BUFFER_SERVICE_SHARED_IMAGE_OHOS_NATIVE_VIDEO_DAWN_REPRESENTATION_H_
#define GPU_COMMAND_BUFFER_SERVICE_SHARED_IMAGE_OHOS_NATIVE_VIDEO_DAWN_REPRESENTATION_H_

#include <errno.h>
#include <dawn/webgpu_cpp.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include <memory>
#include <utility>
#include <vector>

#include "base/files/scoped_file.h"
#include "base/compiler_specific.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/trace_event/trace_event.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#include "gpu/command_buffer/service/shared_image/ozone_image_backing.h"
#include "gpu/command_buffer/service/shared_image/shared_image_representation.h"
#include "ui/ozone/platform/ohos/ohos_native_pixmap.h"  // nogncheck

namespace gpu {
namespace ohos_native_video {

// A decoder queue slot must not be reused if a completion fence was lost.
inline void Quarantine(scoped_refptr<gfx::NativePixmap> pixmap,
                       scoped_refptr<SharedContextState> context) {
  struct Leases {
    base::Lock lock;
    std::vector<scoped_refptr<gfx::NativePixmap>> pixmaps;
  };
  static base::NoDestructor<Leases> leases;
  {
    base::AutoLock lock(leases->lock);
    leases->pixmaps.push_back(std::move(pixmap));
  }
  if (!context->context_lost())
    context->MarkContextLost();
  LOG(ERROR) << "OHOS Dawn native video: completion unknown; decoder lease retained";
}

// Read-lock sync tokens order command submission. Retain the actual decoder
// lease separately until Dawn's GPU completion fence signals, without waiting
// on the GPU main thread or copying any pixels.
inline void Retire(scoped_refptr<gfx::NativePixmap> pixmap,
                   scoped_refptr<SharedContextState> context,
                   gfx::GpuFenceHandle fence) {
  if (fence.is_null())
    return;
  pollfd descriptor{fence.Peek(), POLLIN, 0};
  const int result = poll(&descriptor, 1, 0);
  if (result > 0 && (descriptor.revents & POLLIN) &&
      !(descriptor.revents & (POLLERR | POLLNVAL | POLLHUP))) {
    TRACE_EVENT_INSTANT("gpu", "OhosNativeVideoLeaseCompleted");
    return;
  }
  if (context->context_lost() ||
      (result < 0 && errno != EINTR) ||
      (descriptor.revents & (POLLERR | POLLNVAL | POLLHUP))) {
    Quarantine(std::move(pixmap), std::move(context));
    return;
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&Retire, std::move(pixmap), std::move(context),
                     std::move(fence)),
      base::Milliseconds(4));
}

inline bool Merge(base::ScopedFD& merged, int fd) {
  if (fd == -1)
    return true;  // An already-signaled sync-file fence.
  if (fd < -1)
    return false;
  if (!merged.is_valid()) {
    merged.reset(dup(fd));
    return merged.is_valid();
  }
  sync_merge_data data{};
  data.fd2 = fd;
  if (ioctl(merged.get(), SYNC_IOC_MERGE, &data) != 0)
    return false;
  merged.reset(data.fence);
  return merged.is_valid();
}

}  // namespace ohos_native_video

// Graphite and GL/WebGPU share this native-video ownership protocol.
// Ozone's ordinary Dawn representation has planar views and a single access
// scope. Native decoder YUV instead needs an opaque hardware sampler, and
// exclusive FOREIGN ownership for each submitted Dawn batch. Keep the wire
// texture stable while yielding the allocation back to ANGLE between batches.
class OhosNativeVideoDawnRepresentation final : public DawnImageRepresentation {
 public:
  static std::unique_ptr<OhosNativeVideoDawnRepresentation> Create(
      SharedImageManager* manager,
      OzoneImageBacking* backing,
      MemoryTypeTracker* tracker,
      const wgpu::Device& device,
      scoped_refptr<gfx::NativePixmap> pixmap,
      scoped_refptr<SharedContextState> context,
      wgpu::SharedTextureMemory imported_memory = nullptr) {
    const bool graphite = context && context->IsGraphiteDawnVulkan();
    if (!context || (!context->GrContextIsGL() && !graphite) || !pixmap ||
        !(backing->usage().Has(SHARED_IMAGE_USAGE_WEBGPU_READ) ||
          (graphite && backing->usage().Has(SHARED_IMAGE_USAGE_DISPLAY_READ))) ||
        !device.HasFeature(wgpu::FeatureName::SharedTextureMemoryAHardwareBuffer) ||
        !device.HasFeature(wgpu::FeatureName::SharedFenceSyncFD) ||
        !device.HasFeature(wgpu::FeatureName::OpaqueYCbCrAndroidForExternalTexture)) {
      return nullptr;
    }
    wgpu::SharedTextureMemoryAHardwareBufferDescriptor native;
    native.handle = ui::GetOhosNativeBuffer(*pixmap);
    if (!native.handle)
      return nullptr;
    wgpu::SharedTextureMemoryDescriptor descriptor;
    descriptor.label = "OhosGLDecoderNativeBuffer";
    descriptor.nextInChain = &native;
    auto memory = imported_memory ? std::move(imported_memory)
                                  : device.ImportSharedTextureMemory(&descriptor);
    wgpu::SharedTextureMemoryProperties properties{};
    if (!memory || memory.GetProperties(&properties) != wgpu::Status::Success ||
        properties.format != wgpu::TextureFormat::OpaqueYCbCrAndroid ||
        properties.size.width != static_cast<uint32_t>(backing->size().width()) ||
        properties.size.height != static_cast<uint32_t>(backing->size().height())) {
      return nullptr;
    }
    TRACE_EVENT_INSTANT("gpu", "OhosNativeBufferDawnImport", "format",
                        backing->format().ToString(), "width",
                        backing->size().width(), "height", backing->size().height());
    return std::make_unique<OhosNativeVideoDawnRepresentation>(
        manager, backing, tracker, device, std::move(memory), std::move(pixmap),
        std::move(context));
  }

  OhosNativeVideoDawnRepresentation(
      SharedImageManager* manager,
      OzoneImageBacking* backing,
      MemoryTypeTracker* tracker,
      wgpu::Device device,
      wgpu::SharedTextureMemory memory,
      scoped_refptr<gfx::NativePixmap> pixmap,
      scoped_refptr<SharedContextState> context)
      : DawnImageRepresentation(manager, backing, tracker),
        backing_(backing), device_(std::move(device)), memory_(std::move(memory)),
        pixmap_(std::move(pixmap)), context_(std::move(context)) {}

  ~OhosNativeVideoDawnRepresentation() override { EndAccess(); }

  wgpu::Texture BeginAccess(wgpu::TextureUsage usage,
                            wgpu::TextureUsage internal_usage) override {
    if (texture_ || failed_ || !IsCleared() ||
        (usage & ~wgpu::TextureUsage::TextureBinding) != wgpu::TextureUsage::None ||
        (internal_usage & ~wgpu::TextureUsage::TextureBinding) !=
            wgpu::TextureUsage::None) {
      return nullptr;
    }
    wgpu::TextureDescriptor descriptor;
    descriptor.label = "OhosGLDecoderExternalTexture";
    descriptor.format = wgpu::TextureFormat::OpaqueYCbCrAndroid;
    descriptor.usage = wgpu::TextureUsage::TextureBinding;
    descriptor.dimension = wgpu::TextureDimension::e2D;
    descriptor.mipLevelCount = 1;
    descriptor.sampleCount = 1;
    descriptor.size = {static_cast<uint32_t>(size().width()),
                       static_cast<uint32_t>(size().height()), 1};
    texture_ = memory_.CreateTexture(&descriptor);
    if (!ResumeOhosNativeReadAccess()) {
      texture_.Destroy();
      texture_ = nullptr;
    }
    return texture_;
  }

  bool ResumeOhosNativeReadAccess() override {
    if (access_active_)
      return true;
    if (failed_ || !texture_ || context_->context_lost())
      return false;
    std::vector<gfx::GpuFenceHandle> dependencies;
    bool need_end_fence = false;
    // Exclusive queue-family ownership, even though this consumer only reads.
    if (!backing_->BeginAccess(false, OzoneImageBacking::AccessStream::kWebGPU,
                                &dependencies, need_end_fence)) {
      return false;
    }
    std::vector<wgpu::SharedFence> fences;
    for (const auto& dependency : dependencies) {
      if (dependency.is_null()) {
        Fail();
        return false;
      }
      wgpu::SharedFenceSyncFDDescriptor fd;
      fd.handle = dependency.Peek();  // Dawn duplicates the sync file.
      wgpu::SharedFenceDescriptor descriptor;
      descriptor.nextInChain = &fd;
      fences.push_back(device_.ImportSharedFence(&descriptor));
    }
    std::vector<uint64_t> values(fences.size(), 1);
    wgpu::SharedTextureMemoryVkImageLayoutBeginState layout{};
    layout.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    layout.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    wgpu::SharedTextureMemoryBeginAccessDescriptor access{};
    access.nextInChain = &layout;
    access.initialized = true;
    access.fenceCount = fences.size();
    access.fences = fences.data();
    access.signaledValueCount = values.size();
    access.signaledValues = values.data();
    if (memory_.BeginAccess(texture_, &access) != wgpu::Status::Success) {
      Fail();
      return false;
    }
    access_active_ = true;
    TRACE_EVENT_INSTANT("gpu", "OhosNativeVideoReadResume");
    return true;
  }

  void SuspendOhosNativeReadAccess() override {
    if (!access_active_)
      return;
    access_active_ = false;
    wgpu::SharedTextureMemoryVkImageLayoutEndState layout{};
    layout.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    layout.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    wgpu::SharedTextureMemoryEndAccessState access{};
    access.nextInChain = &layout;
    if (memory_.EndAccess(texture_, &access) != wgpu::Status::Success ||
        layout.newLayout != VK_IMAGE_LAYOUT_GENERAL ||
        access.fenceCount != access.signaledValueCount) {
      Fail();
      return;
    }
    base::ScopedFD completion;
    for (size_t i = 0; i < access.fenceCount; ++i) {
      if (UNSAFE_TODO(access.signaledValues[i]) != 1) {
        Fail();
        return;
      }
      const auto& fence = UNSAFE_TODO(access.fences[i]);
      wgpu::SharedFenceExportInfo info{};
      fence.ExportInfo(&info);
      if (info.type != wgpu::SharedFenceType::SyncFD) {
        Fail();
        return;
      }
      wgpu::SharedFenceSyncFDExportInfo fd{};
      info.nextInChain = &fd;
      fence.ExportInfo(&info);
      if (!ohos_native_video::Merge(completion, fd.handle)) {
        Fail();
        return;
      }
    }
    gfx::GpuFenceHandle completed;
    completed.Adopt(std::move(completion));
    auto retirement = completed.Clone();
    if (!completed.is_null() && retirement.is_null()) {
      Fail();
      return;
    }
    backing_->EndAccess(false, OzoneImageBacking::AccessStream::kWebGPU,
                         std::move(completed));
    ohos_native_video::Retire(pixmap_, context_, std::move(retirement));
    TRACE_EVENT_INSTANT("gpu", "OhosNativeVideoReadSuspend");
  }

  void EndAccess() override {
    SuspendOhosNativeReadAccess();
    if (texture_) {
      texture_.Destroy();
      texture_ = nullptr;
    }
  }

 private:
  void Fail() {
    failed_ = true;
    access_active_ = false;
    ohos_native_video::Quarantine(pixmap_, context_);
  }

  raw_ptr<OzoneImageBacking> backing_;
  wgpu::Device device_;
  wgpu::SharedTextureMemory memory_;
  wgpu::Texture texture_;
  scoped_refptr<gfx::NativePixmap> pixmap_;
  scoped_refptr<SharedContextState> context_;
  bool access_active_ = false;
  bool failed_ = false;
};

}  // namespace gpu

#endif  // GPU_COMMAND_BUFFER_SERVICE_SHARED_IMAGE_OHOS_NATIVE_VIDEO_DAWN_REPRESENTATION_H_
