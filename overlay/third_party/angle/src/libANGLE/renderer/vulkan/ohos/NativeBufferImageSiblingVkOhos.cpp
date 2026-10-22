//
// Copyright 2026 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// NativeBufferImageSiblingVkOhos.cpp:
//    Implements NativeBufferImageSiblingVkOhos.
//

#include "libANGLE/renderer/vulkan/ohos/NativeBufferImageSiblingVkOhos.h"

#include <native_buffer/native_buffer.h>

#include <set>
#include <sstream>

#include "libANGLE/Display.h"
#include "libANGLE/renderer/vulkan/DisplayVk.h"
#include "libANGLE/renderer/vulkan/vk_renderer.h"

namespace rx
{

namespace
{
// From <vulkan/vulkan_ohos.h>, repeated rather than included: the Vulkan
// headers ANGLE builds against are the Khronos ones and do not carry this
// platform's extension, while the driver on the device does.
constexpr VkStructureType kStructureTypeNativeBufferProperties =
    static_cast<VkStructureType>(1000452001);
constexpr VkStructureType kStructureTypeNativeBufferFormatProperties =
    static_cast<VkStructureType>(1000452002);
constexpr VkStructureType kStructureTypeImportNativeBufferInfo =
    static_cast<VkStructureType>(1000452003);
// VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS
constexpr VkExternalMemoryHandleTypeFlags kHandleTypeNativeBuffer = 0x00008000;

struct NativeBufferPropertiesOHOS
{
    VkStructureType sType;
    void *pNext;
    VkDeviceSize allocationSize;
    uint32_t memoryTypeBits;
};

struct NativeBufferFormatPropertiesOHOS
{
    VkStructureType sType;
    void *pNext;
    VkFormat format;
    uint64_t externalFormat;
    VkFormatFeatureFlags formatFeatures;
    VkComponentMapping samplerYcbcrConversionComponents;
    VkSamplerYcbcrModelConversion suggestedYcbcrModel;
    VkSamplerYcbcrRange suggestedYcbcrRange;
    VkChromaLocation suggestedXChromaOffset;
    VkChromaLocation suggestedYChromaOffset;
};

struct ExternalFormatOHOS
{
    VkStructureType sType;
    void *pNext;
    uint64_t externalFormat;
};
constexpr VkStructureType kStructureTypeExternalFormat = static_cast<VkStructureType>(1000452005);

struct ImportNativeBufferInfoOHOS
{
    VkStructureType sType;
    const void *pNext;
    OH_NativeBuffer *buffer;
};

using PFN_GetNativeBufferPropertiesOHOS = VkResult(VKAPI_PTR *)(VkDevice device,
                                                               const OH_NativeBuffer *buffer,
                                                               NativeBufferPropertiesOHOS *pProperties);

OH_NativeBuffer *ClientBufferToNativeBuffer(EGLClientBuffer buffer)
{
    OH_NativeBuffer *nativeBuffer = nullptr;
    if (buffer == nullptr ||
        OH_NativeBuffer_FromNativeWindowBuffer(reinterpret_cast<OHNativeWindowBuffer *>(buffer),
                                               &nativeBuffer) != 0)
    {
        return nullptr;
    }
    return nativeBuffer;
}

angle::Result GetProperties(vk::ErrorContext *context,
                            OH_NativeBuffer *nativeBuffer,
                            NativeBufferPropertiesOHOS *properties,
                            NativeBufferFormatPropertiesOHOS *formatProperties)
{
    VkDevice device = context->getRenderer()->getDevice();
    static PFN_GetNativeBufferPropertiesOHOS getProperties =
        reinterpret_cast<PFN_GetNativeBufferPropertiesOHOS>(
            vkGetDeviceProcAddr(device, "vkGetNativeBufferPropertiesOHOS"));
    ANGLE_VK_CHECK(context, getProperties != nullptr, VK_ERROR_EXTENSION_NOT_PRESENT);

    *formatProperties       = {};
    formatProperties->sType = kStructureTypeNativeBufferFormatProperties;
    *properties             = {};
    properties->sType       = kStructureTypeNativeBufferProperties;
    properties->pNext       = formatProperties;
    ANGLE_VK_TRY(context, getProperties(device, nativeBuffer, properties));
    return angle::Result::Continue;
}

// The VkFormat to import as. The Maleoon driver names one for RGBA but only
// an opaque external format for YUV; these buffers are allocated
// CPU-writable and so laid out linearly as plain NV12 or P010, which is what
// the two-plane formats describe -- the same choice Dawn's import makes.
VkFormat ImportFormat(const NativeBufferFormatPropertiesOHOS &formatProperties, int32_t ohosFormat)
{
    if (formatProperties.format != VK_FORMAT_UNDEFINED)
    {
        return formatProperties.format;
    }
    switch (ohosFormat)
    {
        case NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP:
            return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        case NATIVEBUFFER_PIXEL_FMT_RGBA_8888:
        case NATIVEBUFFER_PIXEL_FMT_RGBX_8888:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case NATIVEBUFFER_PIXEL_FMT_BGRA_8888:
            return VK_FORMAT_B8G8R8A8_UNORM;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

// The YUV-to-RGB conversion the buffer asks for. Chromium tags each video
// buffer with its color space before importing it (the GLES backend's driver
// reads the same tag); without one, the driver's suggestion stands.
void YcbcrConversionFor(OH_NativeBuffer *nativeBuffer,
                        const NativeBufferFormatPropertiesOHOS &formatProperties,
                        VkSamplerYcbcrModelConversion *model,
                        VkSamplerYcbcrRange *range)
{
    *model = formatProperties.suggestedYcbcrModel;
    *range = formatProperties.suggestedYcbcrRange;
    OH_NativeBuffer_ColorSpace colorSpace = OH_COLORSPACE_NONE;
    if (OH_NativeBuffer_GetColorSpace(nativeBuffer, &colorSpace) != 0)
    {
        return;
    }
    switch (colorSpace)
    {
        case OH_COLORSPACE_BT601_EBU_FULL:
        case OH_COLORSPACE_BT601_SMPTE_C_FULL:
        case OH_COLORSPACE_SRGB_FULL:
        case OH_COLORSPACE_P3_FULL:
            *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
            *range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
            break;
        case OH_COLORSPACE_BT601_EBU_LIMIT:
        case OH_COLORSPACE_BT601_SMPTE_C_LIMIT:
        case OH_COLORSPACE_SRGB_LIMIT:
        case OH_COLORSPACE_P3_LIMIT:
            *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
            *range = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
            break;
        case OH_COLORSPACE_BT709_FULL:
            *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
            *range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
            break;
        case OH_COLORSPACE_BT709_LIMIT:
            *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
            *range = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
            break;
        case OH_COLORSPACE_BT2020_HLG_FULL:
        case OH_COLORSPACE_BT2020_PQ_FULL:
            *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
            *range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
            break;
        case OH_COLORSPACE_BT2020_HLG_LIMIT:
        case OH_COLORSPACE_BT2020_PQ_LIMIT:
            *model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
            *range = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
            break;
        default:
            break;
    }
}
}  // namespace

NativeBufferImageSiblingVkOhos::NativeBufferImageSiblingVkOhos(EGLClientBuffer buffer)
    : mBuffer(buffer),
      mNativeBuffer(nullptr),
      mFormat(GL_NONE),
      mRenderable(false),
      mTextureable(false),
      mYUV(false),
      mImage(nullptr)
{}

NativeBufferImageSiblingVkOhos::~NativeBufferImageSiblingVkOhos() {}

// Static
egl::Error NativeBufferImageSiblingVkOhos::ValidateNativeBuffer(vk::Renderer *renderer,
                                                                EGLClientBuffer buffer,
                                                                const egl::AttributeMap &attribs)
{
    if (!renderer->supportsOhosNativeBuffer())
    {
        return egl::Error(EGL_BAD_PARAMETER, "VK_OHOS_external_memory is not enabled.");
    }
    OH_NativeBuffer *nativeBuffer = ClientBufferToNativeBuffer(buffer);
    if (nativeBuffer == nullptr)
    {
        return egl::Error(EGL_BAD_PARAMETER,
                          "The client buffer is not an OHNativeWindowBuffer with a native buffer.");
    }
    OH_NativeBuffer_Config config = {};
    OH_NativeBuffer_GetConfig(nativeBuffer, &config);
    NativeBufferFormatPropertiesOHOS formatProperties = {};
    formatProperties.format                           = VK_FORMAT_UNDEFINED;
    if (ImportFormat(formatProperties, config.format) == VK_FORMAT_UNDEFINED &&
        config.format != NATIVEBUFFER_PIXEL_FMT_YCBCR_P010)
    {
        // Formats the driver does name are only known once it is asked; the
        // ones this port allocates are all covered above.
        std::ostringstream err;
        err << "OH_NativeBuffer pixel format " << config.format << " is not supported.";
        return egl::Error(EGL_BAD_PARAMETER, err.str());
    }
    if (attribs.getAsInt(EGL_PROTECTED_CONTENT_EXT, EGL_FALSE) == EGL_TRUE)
    {
        return egl::Error(EGL_BAD_ACCESS, "Protected OH_NativeBuffers are not supported.");
    }
    return egl::NoError();
}

egl::Error NativeBufferImageSiblingVkOhos::initialize(const egl::Display *display)
{
    DisplayVk *displayVk = vk::GetImpl(display);
    return angle::ToEGL(initImpl(displayVk), EGL_BAD_PARAMETER);
}

angle::Result NativeBufferImageSiblingVkOhos::initImpl(DisplayVk *displayVk)
{
    vk::Renderer *renderer = displayVk->getRenderer();

    mNativeBuffer = ClientBufferToNativeBuffer(mBuffer);
    ANGLE_VK_CHECK(displayVk, mNativeBuffer != nullptr, VK_ERROR_INVALID_EXTERNAL_HANDLE);
    OH_NativeBuffer_Reference(mNativeBuffer);

    OH_NativeBuffer_Config config = {};
    OH_NativeBuffer_GetConfig(mNativeBuffer, &config);
    mSize = gl::Extents(config.width, config.height, 1);

    NativeBufferPropertiesOHOS bufferProperties;
    NativeBufferFormatPropertiesOHOS formatProperties;
    ANGLE_TRY(GetProperties(displayVk, mNativeBuffer, &bufferProperties, &formatProperties));

    const VkFormat importFormat = ImportFormat(formatProperties, config.format);
    // An opaque driver format (P010 on Maleoon: the layout is the driver's,
    // not the two-plane one CPU writes suggest) is sampled through its
    // external format, as Android samples AHardwareBuffers it cannot name.
    const bool isExternal = importFormat == VK_FORMAT_UNDEFINED &&
                            formatProperties.externalFormat != 0 &&
                            (formatProperties.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    ANGLE_VK_CHECK(displayVk, importFormat != VK_FORMAT_UNDEFINED || isExternal,
                   VK_ERROR_FORMAT_NOT_SUPPORTED);

    const vk::Format &vkFormat =
        renderer->getFormat(isExternal ? angle::FormatID::NONE
                                       : vk::GetFormatIDFromVkFormat(importFormat));
    const angle::Format &imageFormat = vkFormat.getActualRenderableImageFormat();
    mFormat = isExternal ? gl::Format(GL_RGB10_A2_EXT) : gl::Format(vkFormat.getIntendedGLFormat());
    mYUV    = isExternal || imageFormat.isYUV;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((config.usage & NATIVEBUFFER_USAGE_HW_TEXTURE) != 0 || mYUV)
    {
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if ((config.usage & NATIVEBUFFER_USAGE_HW_RENDER) != 0 && !mYUV)
    {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }

    vk::YcbcrConversionDesc conversionDesc{};
    if (mYUV)
    {
        ANGLE_VK_CHECK(displayVk, renderer->getFeatures().supportsYUVSamplerConversion.enabled,
                       VK_ERROR_FEATURE_NOT_PRESENT);
        VkSamplerYcbcrModelConversion model;
        VkSamplerYcbcrRange range;
        YcbcrConversionFor(mNativeBuffer, formatProperties, &model, &range);
        const vk::YcbcrLinearFilterSupport linearFilterSupported =
            !isExternal && renderer->hasImageFormatFeatureBits(
                imageFormat.id, VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT)
                ? vk::YcbcrLinearFilterSupport::Supported
                : vk::YcbcrLinearFilterSupport::Unsupported;
        if (isExternal)
        {
            conversionDesc.update(
                renderer, formatProperties.externalFormat, model, range,
                formatProperties.suggestedXChromaOffset, formatProperties.suggestedYChromaOffset,
                vk::kDefaultYCbCrChromaFilter, formatProperties.samplerYcbcrConversionComponents,
                angle::FormatID::NONE,
                (formatProperties.formatFeatures &
                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT) != 0
                    ? vk::YcbcrLinearFilterSupport::Supported
                    : vk::YcbcrLinearFilterSupport::Unsupported);
        }
        else
        {
            // Chromium's buffers are 4:2:0 with chroma sited at the even samples.
            conversionDesc.update(renderer, 0, model, range, VK_CHROMA_LOCATION_COSITED_EVEN,
                                  VK_CHROMA_LOCATION_COSITED_EVEN, vk::kDefaultYCbCrChromaFilter,
                                  formatProperties.samplerYcbcrConversionComponents,
                                  imageFormat.id, linearFilterSupported);
        }
    }

    VkExternalMemoryImageCreateInfo externalMemoryImageCreateInfo = {};
    externalMemoryImageCreateInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalMemoryImageCreateInfo.handleTypes = kHandleTypeNativeBuffer;
    ExternalFormatOHOS externalFormat = {};
    if (isExternal)
    {
        externalFormat.sType                = kStructureTypeExternalFormat;
        externalFormat.externalFormat       = formatProperties.externalFormat;
        externalMemoryImageCreateInfo.pNext = &externalFormat;
        // An external-format image may only be sampled (VUID-VkImageCreateInfo-pNext-02397).
        usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    }

    VkExtent3D vkExtents;
    gl_vk::GetExtent(mSize, &vkExtents);

    mImage = new vk::ImageHelper();
    mImage->setTilingMode(VK_IMAGE_TILING_OPTIMAL);

    const angle::FormatID intendedFormatID = vkFormat.getIntendedFormatID();
    const angle::FormatID actualFormatID =
        vkFormat.getActualImageFormatID(vk::ImageFormatSupport::SampleOnly);
    // YUV images are sampled through their conversion only; RGBA ones may
    // be given sRGB views.
    const vk::ImageFormatReinterpretability formatReinterpretability =
        mYUV ? vk::ImageFormatReinterpretability::None
             : vk::ImageFormatReinterpretability::ColorspaceOverrides;
    VkImageCreateFlags imageCreateFlags = vk::kVkImageCreateFlagsNone;
    VkImageFormatListCreateInfoKHR imageFormatListInfoStorage;
    vk::ImageHelper::ImageFormats imageFormats;
    const void *imageCreateInfoPNext = vk::ImageHelper::DeriveCreateInfoPNext(
        displayVk, actualFormatID, &externalMemoryImageCreateInfo, &imageFormatListInfoStorage,
        &imageFormats, formatReinterpretability, &imageCreateFlags);

    ANGLE_TRY(mImage->initExternal(
        displayVk, gl::TextureType::_2D, vkExtents, intendedFormatID, actualFormatID, 1, usage,
        imageCreateFlags, vk::ImageAccess::ExternalPreInitialized, imageCreateInfoPNext,
        gl::OwnerLevel(0), 1, 1, false, false, vk::TileMemory::Prohibited, conversionDesc,
        nullptr, formatReinterpretability));

    ImportNativeBufferInfoOHOS importInfo = {};
    importInfo.sType                      = kStructureTypeImportNativeBufferInfo;
    importInfo.buffer                     = mNativeBuffer;

    VkMemoryDedicatedAllocateInfo dedicatedAllocInfo = {};
    dedicatedAllocInfo.sType          = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedAllocInfo.pNext          = &importInfo;
    dedicatedAllocInfo.image          = mImage->getImage().getHandle();
    dedicatedAllocInfo.buffer         = VK_NULL_HANDLE;
    const void *dedicatedAllocInfoPtr = &dedicatedAllocInfo;

    VkMemoryRequirements externalMemoryRequirements = {};
    externalMemoryRequirements.size                 = bufferProperties.allocationSize;
    externalMemoryRequirements.alignment            = 0;
    externalMemoryRequirements.memoryTypeBits       = bufferProperties.memoryTypeBits;

    ANGLE_TRY(mImage->initExternalMemory(displayVk, renderer->getMemoryProperties(),
                                         externalMemoryRequirements, 1, &dedicatedAllocInfoPtr,
                                         vk::kForeignDeviceQueueIndex,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));

    {
        // Once per pixel format: what the driver said about it and what the
        // import made of it.
        static std::set<int32_t> logged;
        if (logged.insert(config.format).second)
        {
            WARN() << "OHOS native buffer import: pixel format " << config.format
                   << ", driver format " << formatProperties.format << " external 0x" << std::hex
                   << formatProperties.externalFormat << " features 0x"
                   << formatProperties.formatFeatures << std::dec << " model "
                   << formatProperties.suggestedYcbcrModel << " range "
                   << formatProperties.suggestedYcbcrRange << "; imported as " << importFormat
                   << " YUV " << mYUV << " sampled "
                   << renderer->hasImageFormatFeatureBits(actualFormatID,
                                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
                   << " ycbcr-linear "
                   << renderer->hasImageFormatFeatureBits(
                          actualFormatID,
                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT)
                   << " memoryTypeBits 0x" << std::hex << bufferProperties.memoryTypeBits
                   << std::dec << " size " << bufferProperties.allocationSize;
        }
    }

    if (mYUV)
    {
        mRenderable  = false;
        mTextureable = true;
    }
    else
    {
        mRenderable = renderer->hasImageFormatFeatureBits(actualFormatID,
                                                          VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
        mTextureable = renderer->hasImageFormatFeatureBits(
            actualFormatID,
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    }

    return angle::Result::Continue;
}

void NativeBufferImageSiblingVkOhos::onDestroy(const egl::Display *display)
{
    if (mNativeBuffer != nullptr)
    {
        OH_NativeBuffer_Unreference(mNativeBuffer);
        mNativeBuffer = nullptr;
    }
    ASSERT(mImage == nullptr);
}

gl::Format NativeBufferImageSiblingVkOhos::getFormat() const
{
    return mFormat;
}

bool NativeBufferImageSiblingVkOhos::isRenderable(const gl::Context *context) const
{
    return mRenderable;
}

bool NativeBufferImageSiblingVkOhos::isTexturable(const gl::Context *context) const
{
    return mTextureable;
}

bool NativeBufferImageSiblingVkOhos::isYUV() const
{
    return mYUV;
}

bool NativeBufferImageSiblingVkOhos::hasFrontBufferUsage() const
{
    return false;
}

bool NativeBufferImageSiblingVkOhos::isCubeMap() const
{
    return false;
}

bool NativeBufferImageSiblingVkOhos::hasProtectedContent() const
{
    return false;
}

gl::Extents NativeBufferImageSiblingVkOhos::getSize() const
{
    return mSize;
}

size_t NativeBufferImageSiblingVkOhos::getSamples() const
{
    return 0;
}

uint32_t NativeBufferImageSiblingVkOhos::getLevelCount() const
{
    return 1;
}

vk::ImageHelper *NativeBufferImageSiblingVkOhos::getImage() const
{
    return mImage;
}

void NativeBufferImageSiblingVkOhos::release(vk::Renderer *renderer)
{
    if (mImage != nullptr)
    {
        mImage->releaseImage(renderer);
        mImage->releaseStagedUpdates(renderer);
        SafeDelete(mImage);
    }
}

}  // namespace rx
