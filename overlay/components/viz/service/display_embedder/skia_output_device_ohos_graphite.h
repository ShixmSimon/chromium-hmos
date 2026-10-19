// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef COMPONENTS_VIZ_SERVICE_DISPLAY_EMBEDDER_SKIA_OUTPUT_DEVICE_OHOS_GRAPHITE_H_
#define COMPONENTS_VIZ_SERVICE_DISPLAY_EMBEDDER_SKIA_OUTPUT_DEVICE_OHOS_GRAPHITE_H_

#include "components/viz/service/display_embedder/skia_output_device.h"
#include "gpu/ipc/common/surface_handle.h"

namespace viz {
// Shared phone/tablet Graphite renderer. The phone broker and native-child
// window differ only in buffer acquisition/presentation, never in drawing.
class SkiaOutputDeviceOhosGraphite final : public SkiaOutputDevice {
 public:
  SkiaOutputDeviceOhosGraphite(
      scoped_refptr<gpu::SharedContextState> context_state,
      gpu::SurfaceHandle widget,
      bool requires_alpha,
      gpu::MemoryTracker* memory_tracker,
      DidSwapBufferCompleteCallback callback);
  ~SkiaOutputDeviceOhosGraphite() override;
  bool Reshape(const ReshapeParams& params) override;
  void Submit(scoped_refptr<gpu::SharedContextState> context_state,
              bool sync_cpu,
              base::OnceClosure callback) override;
  void Present(const std::optional<gfx::Rect>& update_rect,
               BufferPresentedCallback feedback,
               OutputSurfaceFrame frame) override;
  void SetPreferredFrameInterval(base::TimeDelta interval) override;

 protected:
  SkSurface* BeginPaint(std::vector<GrBackendSemaphore>* semaphores) override;
  void EndPaint() override;

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace viz
#endif
