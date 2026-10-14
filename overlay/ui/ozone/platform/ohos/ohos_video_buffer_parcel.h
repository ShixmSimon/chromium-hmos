// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_VIDEO_BUFFER_PARCEL_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_VIDEO_BUFFER_PARCEL_H_
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ui {
// The SurfaceBuffer parcel layout is version-specific, not a public ABI.
// Unknown layouts must fall back before sending a buffer to the GPU.
inline bool ValidOhosVideoBufferParcel(const std::vector<uint32_t>& p,
                                       size_t tail_bytes,
                                       size_t fd_count,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t native_format,
                                       uint32_t bytes_per_sample) {
  if (p.size() != 14 || !width || !height || width > 8192 || height > 8192 ||
      (bytes_per_sample != 1 && bytes_per_sample != 2) || p[1] != 1 ||
      p[2] > 6 || p[3] > 256 || p[13] != 1 || fd_count != p[2] + 1 ||
      p[4] != width || p[6] != height || p[8] != native_format ||
      (tail_bytes != p[3] * 4 && tail_bytes != p[3] * 4 + 20) ||
      p[5] < uint64_t{(width + 1) / 2} * 2 * bytes_per_sample ||
      p[5] > 128 * 1024 * 1024 || p[7] > 128 * 1024 * 1024)
    return false;
  const uint64_t pixels = uint64_t{width} * height +
                          uint64_t{(width + 1) / 2} * ((height + 1) / 2) * 2;
  return p[7] >= pixels * bytes_per_sample;
}
}  // namespace ui
#endif
