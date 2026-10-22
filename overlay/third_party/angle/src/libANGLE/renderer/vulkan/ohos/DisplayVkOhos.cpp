//
// Copyright 2026 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// DisplayVkOhos.cpp:
//    Implements the class methods for DisplayVkOhos.
//

#include "libANGLE/renderer/vulkan/ohos/DisplayVkOhos.h"

#include <vulkan/vulkan_ohos.h>
#include <cstring>

#include "libANGLE/renderer/vulkan/ohos/NativeBufferImageSiblingVkOhos.h"
#include "libANGLE/renderer/vulkan/ohos/WindowSurfaceVkOhos.h"
#include "libANGLE/renderer/vulkan/vk_caps_utils.h"
#include "libANGLE/renderer/vulkan/vk_renderer.h"

namespace rx
{

DisplayVkOhos::DisplayVkOhos(const egl::DisplayState &state) : DisplayVk(state) {}

bool DisplayVkOhos::isValidNativeWindow(EGLNativeWindowType window) const
{
    // The shell hands over an OHNativeWindow it is holding open. There is no
    // call to ask the system whether a given pointer is one, so the check
    // that can be made is that there is one -- the same position the Android
    // backend is in.
    return window != 0;
}

SurfaceImpl *DisplayVkOhos::createWindowSurfaceVk(const egl::SurfaceState &state,
                                                  EGLNativeWindowType window)
{
    return new WindowSurfaceVkOhos(state, window);
}

egl::ConfigSet DisplayVkOhos::generateConfigs()
{
    // The formats a window can be presented in. RGBA8 is guaranteed; the
    // others are what this port's UI and video paths ask for, and a config
    // whose format the device does not support is dropped by
    // checkConfigSupport below rather than advertised and then failed.
    constexpr GLenum kColorFormats[] = {GL_RGBA8, GL_RGB8, GL_RGB565, GL_RGB10_A2};

    return egl_vk::GenerateConfigs(kColorFormats, egl_vk::kConfigDepthStencilFormats,
                                   this);
}

void DisplayVkOhos::checkConfigSupport(egl::Config *config)
{
    // Left to the renderer, which knows what the device can present. The
    // Android backend additionally marks configs recordable for screen
    // capture; HarmonyOS records through its own path and needs no flag
    // here.
}

const char *DisplayVkOhos::getWSIExtension() const
{
    return VK_OHOS_SURFACE_EXTENSION_NAME;
}

void DisplayVkOhos::generateExtensions(egl::DisplayExtensions *outExtensions) const
{
    DisplayVk::generateExtensions(outExtensions);
    // OHOS cannot query formats with a null VkSurfaceKHR. These extensions
    // expose the EGL attribute API, not a promise about every native window.
    // WindowSurfaceVk validates the exact format/colorspace pair against the
    // real surface and allocates its swapchain before eglCreateWindowSurface
    // succeeds. Chromium publishes display capability only after that probe.
    for (const char *extension : getRenderer()->getEnabledInstanceExtensions())
    {
        if (std::strcmp(extension, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0)
        {
            outExtensions->glColorspaceDisplayP3 = true;
            outExtensions->glColorspaceDisplayP3Passthrough = true;
            outExtensions->glColorspaceBt2020Pq = true;
            break;
        }
    }
    // Video frames and WebGPU textures arrive as OH_NativeBuffers; with
    // VK_OHOS_external_memory they are imported instead of uploaded.
    outExtensions->imageNativeBufferOHOS = getRenderer()->supportsOhosNativeBuffer();
}

egl::Error DisplayVkOhos::validateImageClientBuffer(const gl::Context *context,
                                                    EGLenum target,
                                                    EGLClientBuffer clientBuffer,
                                                    const egl::AttributeMap &attribs) const
{
    switch (target)
    {
        case EGL_NATIVE_BUFFER_OHOS:
            return NativeBufferImageSiblingVkOhos::ValidateNativeBuffer(getRenderer(), clientBuffer,
                                                                        attribs);

        default:
            return DisplayVk::validateImageClientBuffer(context, target, clientBuffer, attribs);
    }
}

ExternalImageSiblingImpl *DisplayVkOhos::createExternalImageSibling(
    const gl::Context *context,
    EGLenum target,
    EGLClientBuffer buffer,
    const egl::AttributeMap &attribs)
{
    switch (target)
    {
        case EGL_NATIVE_BUFFER_OHOS:
            return new NativeBufferImageSiblingVkOhos(buffer);

        default:
            return DisplayVk::createExternalImageSibling(context, target, buffer, attribs);
    }
}

bool IsVulkanOhosDisplayAvailable()
{
    return true;
}

DisplayImpl *CreateVulkanOhosDisplay(const egl::DisplayState &state)
{
    return new DisplayVkOhos(state);
}

}  // namespace rx
