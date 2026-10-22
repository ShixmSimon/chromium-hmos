//
// Copyright 2026 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// WindowSurfaceVkOhos.h:
//    Defines the class interface for WindowSurfaceVkOhos, implementing WindowSurfaceVk.
//

#ifndef LIBANGLE_RENDERER_VULKAN_OHOS_WINDOWSURFACEVKOHOS_H_
#define LIBANGLE_RENDERER_VULKAN_OHOS_WINDOWSURFACEVKOHOS_H_

#include "libANGLE/renderer/vulkan/SurfaceVk.h"

namespace rx
{

class WindowSurfaceVkOhos : public WindowSurfaceVk
{
  public:
    WindowSurfaceVkOhos(const egl::SurfaceState &surfaceState, EGLNativeWindowType window);

  private:
    angle::Result createSurfaceVk(vk::ErrorContext *context) override;
    angle::Result getCurrentWindowSize(vk::ErrorContext *context,
                                       gl::Extents *extentsOut) const override;
};

}  // namespace rx

#endif  // LIBANGLE_RENDERER_VULKAN_OHOS_WINDOWSURFACEVKOHOS_H_
