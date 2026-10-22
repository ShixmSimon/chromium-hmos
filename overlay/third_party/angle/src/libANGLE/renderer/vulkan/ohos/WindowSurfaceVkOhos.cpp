//
// Copyright 2026 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// WindowSurfaceVkOhos.cpp:
//    Implements the class methods for WindowSurfaceVkOhos.
//

#include "libANGLE/renderer/vulkan/ohos/WindowSurfaceVkOhos.h"

#include "libANGLE/renderer/vulkan/vk_renderer.h"

namespace
{
// From <vulkan/vulkan_ohos.h>, repeated rather than included: the Vulkan
// headers ANGLE builds against are the Khronos ones and do not carry this
// platform's extension, while the loader on the device does.
constexpr VkStructureType kStructureTypeSurfaceCreateInfoOHOS =
    static_cast<VkStructureType>(1000453000);

struct VkSurfaceCreateInfoOHOS
{
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    void *window;  // OHNativeWindow*
};

using PFN_vkCreateSurfaceOHOS = VkResult(VKAPI_PTR *)(
    VkInstance instance,
    const VkSurfaceCreateInfoOHOS *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkSurfaceKHR *pSurface);
}  // namespace

namespace rx
{

WindowSurfaceVkOhos::WindowSurfaceVkOhos(const egl::SurfaceState &surfaceState,
                                         EGLNativeWindowType window)
    : WindowSurfaceVk(surfaceState, window)
{}

angle::Result WindowSurfaceVkOhos::createSurfaceVk(vk::ErrorContext *context)
{
    vk::Renderer *renderer  = context->getRenderer();
    const VkInstance instance = renderer->getInstance();

    static PFN_vkCreateSurfaceOHOS createSurface = reinterpret_cast<
        PFN_vkCreateSurfaceOHOS>(vkGetInstanceProcAddr(instance, "vkCreateSurfaceOHOS"));
    ANGLE_VK_CHECK(context, createSurface != nullptr, VK_ERROR_EXTENSION_NOT_PRESENT);

    VkSurfaceCreateInfoOHOS createInfo = {};
    createInfo.sType                   = kStructureTypeSurfaceCreateInfoOHOS;
    createInfo.flags                   = 0;
    createInfo.window                  = reinterpret_cast<void *>(mNativeWindowType);
    ANGLE_VK_TRY(context, createSurface(instance, &createInfo, nullptr, &mSurface));

    return angle::Result::Continue;
}

angle::Result WindowSurfaceVkOhos::getCurrentWindowSize(vk::ErrorContext *context,
                                                        gl::Extents *extentsOut) const
{
    // Asked of the surface rather than the window, exactly as the Android
    // backend does: the window's own size and the size Vulkan is willing to
    // present can differ, and the swapchain has to follow the latter.
    vk::Renderer *renderer                 = context->getRenderer();
    const VkPhysicalDevice &physicalDevice = renderer->getPhysicalDevice();
    VkSurfaceCapabilitiesKHR surfaceCaps;
    ANGLE_VK_TRY(context, VK_CALL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, physicalDevice,
                                  mSurface, &surfaceCaps));
    *extentsOut = gl::Extents(surfaceCaps.currentExtent.width, surfaceCaps.currentExtent.height, 1);
    return angle::Result::Continue;
}

}  // namespace rx
