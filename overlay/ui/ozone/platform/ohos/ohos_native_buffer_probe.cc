// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/ohos/ohos_native_buffer_probe.h"

#include <dlfcn.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>

#include <string>
#include <vector>

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "ui/gl/gl_bindings.h"
#include "ui/gl/gl_display.h"

// dma-buf import, from EGL_EXT_image_dma_buf_import and
// EGL_EXT_image_dma_buf_import_modifiers, should the headers lack them.
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define EGL_DMA_BUF_PLANE1_FD_EXT 0x3275
#define EGL_DMA_BUF_PLANE1_OFFSET_EXT 0x3276
#define EGL_DMA_BUF_PLANE1_PITCH_EXT 0x3277
#define EGL_YUV_COLOR_SPACE_HINT_EXT 0x327B
#define EGL_SAMPLE_RANGE_HINT_EXT 0x327C
#define EGL_ITU_REC601_EXT 0x327F
#define EGL_ITU_REC2020_EXT 0x3281
#define EGL_YUV_NARROW_RANGE_EXT 0x3283
#endif

namespace ui {
namespace {

// HarmonyOS's EGL target for an OHNativeWindowBuffer, as ArkWeb uses it.
constexpr EGLenum kEglNativeBufferOhos = 0x34E1;

// DRM fourcc codes.
constexpr EGLint kFourccP010 = 0x30313050;  // 'P' '0' '1' '0'
constexpr EGLint kFourccNv12 = 0x3231564E;  // 'N' 'V' '1' '2'

// OH_NativeBuffer_Format values (buffer_common.h).
constexpr int32_t kFormatNv12 = 24;  // NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP
constexpr int32_t kFormatP010 = 35;  // NATIVEBUFFER_PIXEL_FMT_YCBCR_P010

constexpr int32_t kWidth = 1920;
constexpr int32_t kHeight = 1080;

std::string ImageExtensions(const char* all) {
  std::string found;
  if (!all) {
    return "(none)";
  }
  for (const std::string& extension : base::SplitString(
           all, " ", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (extension.find("image") != std::string::npos ||
        extension.find("dma") != std::string::npos ||
        extension.find("native") != std::string::npos ||
        extension.find("buffer") != std::string::npos ||
        extension.find("external") != std::string::npos ||
        extension.find("OHOS") != std::string::npos) {
      found += " " + extension;
    }
  }
  return found;
}

// The system EGL, as ANGLE's GLES backend loads it.
struct SystemEgl {
  EGLDisplay (*get_display)(EGLNativeDisplayType) = nullptr;
  EGLBoolean (*initialize)(EGLDisplay, EGLint*, EGLint*) = nullptr;
  const char* (*query_string)(EGLDisplay, EGLint) = nullptr;
  EGLImageKHR (*create_image)(EGLDisplay,
                              EGLContext,
                              EGLenum,
                              EGLClientBuffer,
                              const EGLint*) = nullptr;
  EGLBoolean (*destroy_image)(EGLDisplay, EGLImageKHR) = nullptr;
  EGLint (*get_error)() = nullptr;
  EGLDisplay display = EGL_NO_DISPLAY;
};

SystemEgl LoadSystemEgl() {
  SystemEgl egl;
  void* library = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    return egl;
  }
  egl.get_display = reinterpret_cast<decltype(egl.get_display)>(
      dlsym(library, "eglGetDisplay"));
  egl.initialize = reinterpret_cast<decltype(egl.initialize)>(
      dlsym(library, "eglInitialize"));
  egl.query_string = reinterpret_cast<decltype(egl.query_string)>(
      dlsym(library, "eglQueryString"));
  egl.create_image = reinterpret_cast<decltype(egl.create_image)>(
      dlsym(library, "eglCreateImageKHR"));
  egl.destroy_image = reinterpret_cast<decltype(egl.destroy_image)>(
      dlsym(library, "eglDestroyImageKHR"));
  egl.get_error =
      reinterpret_cast<decltype(egl.get_error)>(dlsym(library, "eglGetError"));
  if (egl.get_display && egl.initialize) {
    egl.display = egl.get_display(EGL_DEFAULT_DISPLAY);
    if (egl.display != EGL_NO_DISPLAY &&
        !egl.initialize(egl.display, nullptr, nullptr)) {
      egl.display = EGL_NO_DISPLAY;
    }
  }
  return egl;
}

struct ProbeBuffer {
  OH_NativeBuffer* buffer = nullptr;
  OHNativeWindowBuffer* window_buffer = nullptr;
  int fd = -1;
  OH_NativeBuffer_Planes planes = {};
};

ProbeBuffer AllocateProbeBuffer(const char* label, int32_t format) {
  ProbeBuffer probe;
  OH_NativeBuffer_Config config = {};
  config.width = kWidth;
  config.height = kHeight;
  config.format = format;
  config.usage = NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE |
                 NATIVEBUFFER_USAGE_MEM_DMA | NATIVEBUFFER_USAGE_HW_TEXTURE;
  probe.buffer = OH_NativeBuffer_Alloc(&config);
  if (!probe.buffer) {
    LOG(WARNING) << "OHOS native buffer probe: " << label
                 << " allocation failed";
    return probe;
  }
  OH_NativeBuffer_Config allocated = {};
  OH_NativeBuffer_GetConfig(probe.buffer, &allocated);

  void* address = nullptr;
  const int32_t mapped =
      OH_NativeBuffer_MapPlanes(probe.buffer, &address, &probe.planes);
  std::string planes;
  if (mapped == 0) {
    for (uint32_t i = 0; i < probe.planes.planeCount && i < 4; ++i) {
      planes += " [offset " +
                base::NumberToString(probe.planes.planes[i].offset) +
                " row " +
                base::NumberToString(probe.planes.planes[i].rowStride) +
                " column " +
                base::NumberToString(probe.planes.planes[i].columnStride) +
                "]";
    }
    OH_NativeBuffer_Unmap(probe.buffer);
  }

  probe.window_buffer =
      OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer(probe.buffer);
  BufferHandle* handle =
      probe.window_buffer
          ? OH_NativeWindow_GetBufferHandleFromNative(probe.window_buffer)
          : nullptr;
  if (handle) {
    probe.fd = handle->fd;
  }
  LOG(WARNING) << "OHOS native buffer probe: " << label << " allocated "
               << allocated.width << "x" << allocated.height << " format "
               << allocated.format << " stride " << allocated.stride
               << "; map planes " << (mapped == 0 ? "ok" : "failed") << " ("
               << probe.planes.planeCount << ")" << planes << "; handle "
               << (handle ? "fd " + base::NumberToString(handle->fd) +
                                " stride " +
                                base::NumberToString(handle->stride) +
                                " size " + base::NumberToString(handle->size) +
                                " format " +
                                base::NumberToString(handle->format)
                          : std::string("none"));
  return probe;
}

void ReleaseProbeBuffer(ProbeBuffer& probe) {
  if (probe.window_buffer) {
    OH_NativeWindow_DestroyNativeWindowBuffer(probe.window_buffer);
  }
  if (probe.buffer) {
    OH_NativeBuffer_Unreference(probe.buffer);
  }
}

// Two-plane YUV through dma-buf, with the chroma plane where MapPlanes says.
std::vector<EGLint> DmaBufAttributes(const ProbeBuffer& probe,
                                     EGLint fourcc,
                                     bool rec2020) {
  const uint32_t chroma = probe.planes.planeCount > 1 ? 1 : 0;
  return {EGL_WIDTH,
          kWidth,
          EGL_HEIGHT,
          kHeight,
          EGL_LINUX_DRM_FOURCC_EXT,
          fourcc,
          EGL_DMA_BUF_PLANE0_FD_EXT,
          probe.fd,
          EGL_DMA_BUF_PLANE0_OFFSET_EXT,
          static_cast<EGLint>(probe.planes.planes[0].offset),
          EGL_DMA_BUF_PLANE0_PITCH_EXT,
          static_cast<EGLint>(probe.planes.planes[0].rowStride),
          EGL_DMA_BUF_PLANE1_FD_EXT,
          probe.fd,
          EGL_DMA_BUF_PLANE1_OFFSET_EXT,
          static_cast<EGLint>(probe.planes.planes[chroma].offset),
          EGL_DMA_BUF_PLANE1_PITCH_EXT,
          static_cast<EGLint>(probe.planes.planes[chroma].rowStride),
          EGL_YUV_COLOR_SPACE_HINT_EXT,
          rec2020 ? EGL_ITU_REC2020_EXT : EGL_ITU_REC601_EXT,
          EGL_SAMPLE_RANGE_HINT_EXT,
          EGL_YUV_NARROW_RANGE_EXT,
          EGL_NONE};
}

void TryAngleDmaBuf(gl::GLDisplayEGL* display,
                    const char* label,
                    const ProbeBuffer& probe,
                    EGLint fourcc,
                    bool rec2020) {
  if (probe.fd < 0 || probe.planes.planeCount == 0) {
    LOG(WARNING) << "OHOS native buffer probe: ANGLE dma-buf " << label
                 << " skipped (no fd or planes)";
    return;
  }
  const std::vector<EGLint> attributes =
      DmaBufAttributes(probe, fourcc, rec2020);
  EGLImageKHR image =
      eglCreateImageKHR(display->GetDisplay(), EGL_NO_CONTEXT,
                        EGL_LINUX_DMA_BUF_EXT, nullptr, attributes.data());
  const EGLint error = image == EGL_NO_IMAGE_KHR ? eglGetError() : EGL_SUCCESS;
  LOG(WARNING) << "OHOS native buffer probe: ANGLE dma-buf " << label << " "
               << (image != EGL_NO_IMAGE_KHR ? "imported" : "failed")
               << " (error " << error << ")";
  if (image != EGL_NO_IMAGE_KHR) {
    eglDestroyImageKHR(display->GetDisplay(), image);
  }
}

void TrySystemNativeBuffer(SystemEgl& egl,
                           const char* label,
                           const ProbeBuffer& probe) {
  if (!probe.window_buffer || egl.display == EGL_NO_DISPLAY ||
      !egl.create_image || !egl.destroy_image) {
    return;
  }
  const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
  EGLImageKHR image = egl.create_image(
      egl.display, EGL_NO_CONTEXT, kEglNativeBufferOhos,
      static_cast<EGLClientBuffer>(probe.window_buffer), attributes);
  const EGLint error = image == EGL_NO_IMAGE_KHR && egl.get_error
                           ? egl.get_error()
                           : EGL_SUCCESS;
  LOG(WARNING) << "OHOS native buffer probe: system EGL_NATIVE_BUFFER_OHOS "
               << label << " "
               << (image != EGL_NO_IMAGE_KHR ? "imported" : "failed")
               << " (error " << error << ")";
  if (image != EGL_NO_IMAGE_KHR) {
    egl.destroy_image(egl.display, image);
  }
}

}  // namespace

void ProbeOhosNativeBufferImport(gl::GLDisplayEGL* display) {
  static bool probed = false;
  if (probed || !display || display->GetDisplay() == EGL_NO_DISPLAY) {
    return;
  }
  probed = true;

  LOG(WARNING) << "OHOS native buffer probe: ANGLE EGL extensions ["
               << ImageExtensions(
                      eglQueryString(display->GetDisplay(), EGL_EXTENSIONS))
               << " ]";
  SystemEgl egl = LoadSystemEgl();
  LOG(WARNING) << "OHOS native buffer probe: system EGL extensions ["
               << (egl.display != EGL_NO_DISPLAY && egl.query_string
                       ? ImageExtensions(
                             egl.query_string(egl.display, EGL_EXTENSIONS))
                       : std::string("(no display)"))
               << " ]";

  ProbeBuffer p010 = AllocateProbeBuffer("P010", kFormatP010);
  ProbeBuffer nv12 = AllocateProbeBuffer("NV12", kFormatNv12);

  if (display->ext->b_EGL_KHR_image_base) {
    TryAngleDmaBuf(display, "P010 Rec.2020", p010, kFourccP010, true);
    TryAngleDmaBuf(display, "NV12 Rec.601", nv12, kFourccNv12, false);
    // Not EGL_NATIVE_BUFFER_OHOS through ANGLE: ANGLE does not know the
    // target, and asking it crashed the browser at startup.
  } else {
    LOG(WARNING) << "OHOS native buffer probe: ANGLE has no EGL_KHR_image_base";
  }
  TrySystemNativeBuffer(egl, "P010", p010);
  TrySystemNativeBuffer(egl, "NV12", nv12);

  ReleaseProbeBuffer(p010);
  ReleaseProbeBuffer(nv12);
}

}  // namespace ui
