// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_PIXMAP_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_PIXMAP_H_

#include <memory>
#include <optional>

#include "components/viz/common/resources/shared_image_format.h"
#include "ui/gfx/buffer_types.h"
#include "ui/gfx/client_native_pixmap_factory.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/native_pixmap.h"

namespace gfx {
class ColorSpace;
}

namespace ui {

class NativePixmapGLBinding;

// Native pixmaps on HarmonyOS: OH_NativeBuffers, shared as dma-buf fds.
//
// Software-decoded video is written into these by the renderer and sampled
// by the GPU without a per-frame texture upload, which for 10-bit 1080p had
// cost the GPU thread 5-6 ms a frame. Only the format 10-bit video uses is
// offered -- P010 -- sampled as one external texture with the driver
// converting it, since GLES here has no R16 for per-plane P010.
//
// HarmonyOS has no public way back from a dma-buf fd to its OH_NativeBuffer,
// which EGL_NATIVE_BUFFER_OHOS needs. The GPU process allocates the buffers
// and imports them itself, so it keeps each one in a registry, and a handle
// coming back to it is resolved there by the name it gave the dma-buf: the
// inode would be the natural key, but HarmonyOS reports 0 for it, and kcmp
// is not allowed in the app sandbox. A
// buffer stays registered for a while after its last pixmap goes, in case
// its handle is still on the way. Renderers, processes of their own, only
// write into the buffer: they map the dma-buf fd directly.

// Whether `format` is one this platform allocates and imports.
bool IsOhosNativePixmapFormat(viz::SharedImageFormat format);

scoped_refptr<gfx::NativePixmap> CreateOhosNativePixmap(
    gfx::Size size,
    viz::SharedImageFormat format);

scoped_refptr<gfx::NativePixmap> CreateOhosNativePixmapFromHandle(
    gfx::Size size,
    viz::SharedImageFormat format,
    gfx::NativePixmapHandle handle);

// Imports `pixmap` as an EGLImage through EGL_NATIVE_BUFFER_OHOS, tagged with
// `color_space` so the driver converts YUV with the right matrix and range,
// and binds it to `texture_id` on `target` (GL_TEXTURE_EXTERNAL_OES).
std::unique_ptr<NativePixmapGLBinding> ImportOhosNativePixmap(
    scoped_refptr<gfx::NativePixmap> pixmap,
    const gfx::ColorSpace& color_space,
    unsigned int target,
    unsigned int texture_id);

std::unique_ptr<gfx::ClientNativePixmapFactory>
CreateOhosClientNativePixmapFactory();

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_PIXMAP_H_
