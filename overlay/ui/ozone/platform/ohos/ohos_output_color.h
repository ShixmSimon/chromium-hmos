// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_OUTPUT_COLOR_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_OUTPUT_COLOR_H_
#include <cstdint>

namespace ui {
// Wire values are versioned with the phone shared-buffer protocol. Luminance
// is in integer millinits, never ABI-dependent native metadata structures.
enum class OhosOutputMode : uint32_t { kSdr8, kSdr10, kHdr10 };
struct OhosOutputColor {
  OhosOutputMode mode = OhosOutputMode::kSdr8;
  uint32_t white_millinits = 0;
  uint32_t peak_millinits = 0;
  constexpr bool operator==(const OhosOutputColor& other) const {
    return mode == other.mode && white_millinits == other.white_millinits &&
           peak_millinits == other.peak_millinits;
  }
  constexpr bool operator!=(const OhosOutputColor& other) const {
    return !(*this == other);
  }
};
// The phone's HDR output also carries SDR/WCG frames. Their encoded pixels
// cannot exceed SDR white: do not advertise unused display headroom as content
// light levels to the system tone mapper. Keep the display capability snapshot
// separate so an ensuing HDR frame can use the available range again.
constexpr OhosOutputColor OhosOutputColorForContent(OhosOutputColor display,
                                                    bool has_hdr_content) {
  if (display.mode == OhosOutputMode::kHdr10 && !has_hdr_content)
    display.peak_millinits = display.white_millinits;
  return display;
}
constexpr bool ValidOhosOutputColor(const OhosOutputColor& color) {
  if (color.mode == OhosOutputMode::kSdr8 ||
      color.mode == OhosOutputMode::kSdr10)
    return color.white_millinits == 0 && color.peak_millinits == 0;
  return color.mode == OhosOutputMode::kHdr10 && color.white_millinits > 0 &&
         color.white_millinits <= 10000000 &&
         color.peak_millinits >= color.white_millinits &&
         color.peak_millinits <= 10000000;
}
constexpr uint32_t OhosOutputNativeFormat(const OhosOutputColor& color) {
  return color.mode == OhosOutputMode::kSdr8 ? 12u : 34u;
}
}  // namespace ui
#endif
