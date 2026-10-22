//
// Copyright 2026 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// NativeBufferImageSiblingVkOhos.h:
//    Defines NativeBufferImageSiblingVkOhos, which wraps EGL images created
//    from HarmonyOS native buffers (EGL_NATIVE_BUFFER_OHOS).
//

#ifndef LIBANGLE_RENDERER_VULKAN_OHOS_NATIVEBUFFERIMAGESIBLINGVKOHOS_H_
#define LIBANGLE_RENDERER_VULKAN_OHOS_NATIVEBUFFERIMAGESIBLINGVKOHOS_H_

#include "libANGLE/renderer/vulkan/ImageVk.h"

struct OH_NativeBuffer;

namespace rx
{

// The Vulkan backend's counterpart of the GLES backend's EGL_NATIVE_BUFFER_OHOS
// passthrough: the client buffer is an OHNativeWindowBuffer, imported with
// VK_OHOS_external_memory the way HardwareBufferImageSiblingVkAndroid imports
// an AHardwareBuffer.
class NativeBufferImageSiblingVkOhos : public ExternalImageSiblingVk
{
  public:
    NativeBufferImageSiblingVkOhos(EGLClientBuffer buffer);
    ~NativeBufferImageSiblingVkOhos() override;

    static egl::Error ValidateNativeBuffer(vk::Renderer *renderer,
                                           EGLClientBuffer buffer,
                                           const egl::AttributeMap &attribs);

    egl::Error initialize(const egl::Display *display) override;
    void onDestroy(const egl::Display *display) override;

    // ExternalImageSiblingImpl interface
    gl::Format getFormat() const override;
    bool isRenderable(const gl::Context *context) const override;
    bool isTexturable(const gl::Context *context) const override;
    bool isYUV() const override;
    bool hasFrontBufferUsage() const override;
    bool isCubeMap() const override;
    bool hasProtectedContent() const override;
    gl::Extents getSize() const override;
    size_t getSamples() const override;
    uint32_t getLevelCount() const override;

    // ExternalImageSiblingVk interface
    vk::ImageHelper *getImage() const override;

    void release(vk::Renderer *renderer) override;

  private:
    angle::Result initImpl(DisplayVk *displayVk);

    EGLClientBuffer mBuffer;
    OH_NativeBuffer *mNativeBuffer;
    gl::Extents mSize;
    gl::Format mFormat;

    bool mRenderable;
    bool mTextureable;
    bool mYUV;

    vk::ImageHelper *mImage;
};

}  // namespace rx

#endif  // LIBANGLE_RENDERER_VULKAN_OHOS_NATIVEBUFFERIMAGESIBLINGVKOHOS_H_
