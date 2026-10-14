// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_PIXMAP_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_PIXMAP_H_

#include <memory>
#include <optional>

#include "base/functional/callback.h"
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
// cost the GPU thread 5-6 ms a frame, and 8-bit video a copy a frame too.
// The formats video uses -- NV12 and P010 -- are each sampled as one
// external texture with the driver converting it, since GLES here has no R16
// for per-plane P010. RGBA and BGRA are allocated for WebGPU, whose textures
// are shared between Dawn's Vulkan device and the GLES compositor; renderers
// are offered only the video formats (IsNativePixmapConfigSupported).
//
// HarmonyOS has no public way back from a dma-buf fd to its OH_NativeBuffer,
// which EGL_NATIVE_BUFFER_OHOS needs. The GPU process allocates the buffers
// and imports them itself, so it keeps each one in a registry, and a handle
// coming back to it is resolved there by an id written into the padding
// after the buffer's last plane: the dma-buf's inode would be the natural
// key, but HarmonyOS reports 0 for it, kcmp is not allowed in the app
// sandbox, and a dma-buf's name does not show in its fdinfo. A
// buffer stays registered for a while after its last pixmap goes, in case
// its handle is still on the way. Renderers, processes of their own, only
// write into the buffer: they map the dma-buf fd directly.
// Idle entries are retired after a ten-second handoff grace period, even if
// no more buffers are allocated. Re-importing one restarts that period on its
// next release. Memory pressure never shortens the handoff grace period.

// Whether `format` is one this platform allocates and imports.
bool IsOhosNativePixmapFormat(viz::SharedImageFormat format);

scoped_refptr<gfx::NativePixmap> CreateOhosNativePixmap(
    gfx::Size size,
    viz::SharedImageFormat format);

scoped_refptr<gfx::NativePixmap> CreateOhosNativePixmapFromHandle(
    gfx::Size size,
    viz::SharedImageFormat format,
    gfx::NativePixmapHandle handle);

// Wraps an acquired decoder surface buffer in the GPU process. The caller
// transfers a release closure that owns the buffer/consumer queue. No mapping,
// pixel copy, writable tag, or cross-process dma-buf re-identification is used.
// The returned pixmap is GPU-only; only its SharedImage mailbox is exported.
// If the decoder supplies a known buffer color space, replace *color_space;
// otherwise preserve the caller's stream color space. Call before GPU import.
scoped_refptr<gfx::NativePixmap> CreateOhosVideoNativePixmap(
    void* window_buffer,
    gfx::ColorSpace* color_space,
    base::OnceClosure release);

// Stable allocation identity, not the address of a per-acquisition wrapper.
// Only decoder surface pixmaps have one. Scope it to the decoder's epoch.
std::optional<uint32_t> GetOhosVideoNativePixmapId(
    const gfx::NativePixmap& pixmap);

// Holds the allocation through an independent native wrapper, without holding
// the frame's ConsumerSurface lease. Safe to retain in an import cache; the
// original pixmap must separately survive until that frame's GPU reads finish.
scoped_refptr<gfx::NativePixmap> CloneOhosVideoNativePixmapForImport(
    const gfx::NativePixmap& pixmap);

// Browser -> fork GPU transfer of decoder-owned NativeBuffers. The optional
// release endpoint is kept by GPU imports until their last completed read.
gfx::NativePixmapHandle ExportOhosVideoNativePixmap(
    const gfx::NativePixmap& pixmap,
    base::ScopedFD release_endpoint);
scoped_refptr<gfx::NativePixmap> ImportOhosVideoNativePixmap(
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

// The OH_NativeBuffer behind `pixmap` if this process allocated it, else
// null; for Dawn, which imports it with VK_OHOS_external_memory
// (DawnOzoneImageRepresentation). An OH_NativeBuffer*, untyped so that the
// GPU service needs no HarmonyOS header to call it.
void* GetOhosNativeBuffer(const gfx::NativePixmap& pixmap);

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_OHOS_OHOS_NATIVE_PIXMAP_H_
