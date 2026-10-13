// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "ui/ozone/platform/ohos/ohos_phone_gpu_presenter.h"

#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>
#include <poll.h>
#include <unistd.h>

#include <cstring>

namespace ui::phone_gpu {
bool Present(void* target, const Frame& frame, const void* pixels) {
  if (!target || !pixels || !FrameBytes(frame))
    return false;
  auto* window = static_cast<OHNativeWindow*>(target);
  constexpr int32_t kRgba8888 = 12;
  const uint64_t usage =
      NATIVEBUFFER_USAGE_CPU_WRITE | NATIVEBUFFER_USAGE_CPU_READ |
      NATIVEBUFFER_USAGE_HW_TEXTURE | NATIVEBUFFER_USAGE_MEM_DMA;
  if (OH_NativeWindow_NativeWindowHandleOpt(window, SET_FORMAT, kRgba8888) ||
      OH_NativeWindow_NativeWindowHandleOpt(window, SET_USAGE, usage) ||
      OH_NativeWindow_NativeWindowHandleOpt(
          window, SET_BUFFER_GEOMETRY, static_cast<int32_t>(frame.width),
          static_cast<int32_t>(frame.height)) ||
      OH_NativeWindow_NativeWindowHandleOpt(window, SET_TIMEOUT, int32_t{200}))
    return false;

  OHNativeWindowBuffer* buffer = nullptr;
  int fence = -1;
  if (OH_NativeWindow_NativeWindowRequestBuffer(window, &buffer, &fence) != 0)
    return false;
  bool ready = true;
  if (fence >= 0) {
    pollfd item{fence, POLLIN, 0};
    ready = poll(&item, 1, 200) > 0 && (item.revents & POLLIN);
    close(fence);
  }
  OH_NativeBuffer* native = nullptr;
  void* address = nullptr;
  bool copied = false;
  if (ready && buffer &&
      OH_NativeBuffer_FromNativeWindowBuffer(buffer, &native) == 0 && native) {
    OH_NativeBuffer_Config config{};
    OH_NativeBuffer_GetConfig(native, &config);
    const auto* handle = OH_NativeWindow_GetBufferHandleFromNative(buffer);
    const size_t row = static_cast<size_t>(frame.width) * 4;
    if (config.width == static_cast<int32_t>(frame.width) &&
        config.height == static_cast<int32_t>(frame.height) &&
        config.format == kRgba8888 &&
        config.stride >= static_cast<int32_t>(row) && handle &&
        handle->size > 0 &&
        uint64_t{frame.height} * config.stride <=
            static_cast<uint64_t>(handle->size) &&
        OH_NativeBuffer_Map(native, &address) == 0 && address) {
      // Legacy pbuffer rows are bottom-up; the shared-output FBO is top-left.
      for (uint32_t y = 0; y < frame.height; ++y) {
        memcpy(
            static_cast<char*>(address) + size_t{y} * config.stride,
            static_cast<const char*>(pixels) +
                ((frame.reserved & kTopLeftRows) ? y : frame.height - y - 1) *
                    row,
            row);
      }
      copied = OH_NativeBuffer_Unmap(native) == 0;
    }
  }
  if (!copied) {
    if (buffer)
      OH_NativeWindow_NativeWindowAbortBuffer(window, buffer);
    return false;
  }
  Region damage{nullptr, 0};
  const int result =
      OH_NativeWindow_NativeWindowFlushBuffer(window, buffer, -1, damage);
  if (result)
    OH_NativeWindow_NativeWindowAbortBuffer(window, buffer);
  return result == 0;
}
}  // namespace ui::phone_gpu
