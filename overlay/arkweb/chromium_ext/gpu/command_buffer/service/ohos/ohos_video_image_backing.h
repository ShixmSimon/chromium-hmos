// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GPU_COMMAND_BUFFER_SERVICE_OHOS_OHOS_VIDEO_IMAGE_BACKING_H_
#define GPU_COMMAND_BUFFER_SERVICE_OHOS_OHOS_VIDEO_IMAGE_BACKING_H_

#include <memory>

#include "gpu/command_buffer/service/ohos/ohos_image_backing.h"
#include "gpu/command_buffer/service/ohos/native_image_texture_owner.h"
#include "gpu/gpu_gles2_export.h"
#include "gpu/ipc/common/vulkan_ycbcr_info.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "ui/gl/ohos/native_buffer_utils.h"

namespace viz {
class VulkanContextProvider;
}  // namespace viz

namespace gpu {
struct Mailbox;
class AbstractTextureOHOS;
class RefCountedLock;
class StreamTextureSharedImageInterface;
class SharedContextState;
class ScopedNativeBufferFenceSync;
class NativeImageTextureOwner;

class GPU_GLES2_EXPORT OhosVideoImageBacking : public OhosImageBacking {
 public:
  static std::unique_ptr<OhosVideoImageBacking> Create(
      const Mailbox& mailbox,
      const gfx::Size& size,
      const gfx::ColorSpace color_space,
      GrSurfaceOrigin surface_origin,
      SkAlphaType alpha_type,
      std::string debug_label,
      gl::ohos::TextureOwnerMode texture_owner_mode,
      scoped_refptr<StreamTextureSharedImageInterface> stream_texture_sii,
      scoped_refptr<SharedContextState> context_state,
      scoped_refptr<RefCountedLock> drdc_lock);

  // Returns ycbcr information. This is only valid in vulkan context and
  // nullopt for other context.
  static absl::optional<VulkanYCbCrInfo> GetYcbcrInfo(
      NativeImageTextureOwner* texture_owner,
      gpu::VulkanContextProvider* vulkan_context_provider);

  ~OhosVideoImageBacking() override;

  OhosVideoImageBacking(const OhosVideoImageBacking&) = delete;
  OhosVideoImageBacking& operator=(const OhosVideoImageBacking&) = delete;

  SharedImageBackingType GetType() const override;
  gfx::Rect ClearedRect() const override;
  void SetClearedRect(const gfx::Rect& cleared_rect) override;
  void Update(gfx::GpuFenceHandle in_fence) override;

 protected:
  OhosVideoImageBacking(const Mailbox& mailbox,
                        const gfx::Size& size,
                        const gfx::ColorSpace color_space,
                        GrSurfaceOrigin surface_origin,
                        SkAlphaType alpha_type,
                        std::string debug_label,
                        bool is_thread_safe);

  std::unique_ptr<AbstractTextureOHOS> GenAbstractTexture(
      const bool passthrough);
};

}  // namespace gpu

#endif  // GPU_COMMAND_BUFFER_SERVICE_OHOS_OHOS_VIDEO_IMAGE_BACKING_H_
