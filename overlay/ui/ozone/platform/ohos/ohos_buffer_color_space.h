// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_BUFFER_COLOR_SPACE_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_BUFFER_COLOR_SPACE_H_

#include <native_buffer/buffer_common.h>

namespace ui {
// buffer_common.h explicitly defines these display aliases as equivalent.
// GetColorSpace may return the alias after SetColorSpace used the full name.
// Normalize only the output spaces we request; do not conflate range,
// transfer function or primaries merely because both values describe RGB.
constexpr OH_NativeBuffer_ColorSpace CanonicalOhosOutputColorSpace(
    OH_NativeBuffer_ColorSpace space) {
  switch (space) {
    case OH_COLORSPACE_DISPLAY_SRGB:
      return OH_COLORSPACE_SRGB_FULL;
    case OH_COLORSPACE_DISPLAY_P3_SRGB:
      return OH_COLORSPACE_P3_FULL;
    case OH_COLORSPACE_DISPLAY_BT2020_PQ:
      return OH_COLORSPACE_BT2020_PQ_FULL;
    default:
      return space;
  }
}
constexpr bool IsOhosBufferColorReadbackValid(
    int result,
    OH_NativeBuffer_ColorSpace expected,
    OH_NativeBuffer_ColorSpace actual) {
  return result == 0 && expected != OH_COLORSPACE_NONE &&
         CanonicalOhosOutputColorSpace(actual) ==
             CanonicalOhosOutputColorSpace(expected);
}
}  // namespace ui
#endif
