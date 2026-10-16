// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_OUTPUT_COLOR_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_OUTPUT_COLOR_H_
#include <cstdint>

namespace ui {
// Wire values are versioned with the phone shared-buffer protocol. Luminance
// is in integer millinits, never ABI-dependent native metadata structures.
enum class OhosOutputMode : uint32_t { kSdr8, kSdr10, kHdr10, kP3Sdr10 };
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
constexpr bool ValidOhosOutputColor(const OhosOutputColor& color) {
  if (color.mode == OhosOutputMode::kSdr8 ||
      color.mode == OhosOutputMode::kSdr10 ||
      color.mode == OhosOutputMode::kP3Sdr10)
    return color.white_millinits == 0 && color.peak_millinits == 0;
  return color.mode == OhosOutputMode::kHdr10 && color.white_millinits > 0 &&
         color.white_millinits <= 10000000 &&
         color.peak_millinits >= color.white_millinits &&
         color.peak_millinits <= 10000000;
}
// Only switch to formats actually probed by this output. P3 is SDR and
// never borrows HDR metadata or brightness headroom.
constexpr bool CanSwitchOhosPhoneOutputColor(const OhosOutputColor& from,
                                             const OhosOutputColor& to,
                                             bool hdr_capable,
                                             bool p3_capable = false) {
  const auto supported = [&](OhosOutputMode mode) {
    return mode == OhosOutputMode::kSdr10 ||
           (mode == OhosOutputMode::kHdr10 && hdr_capable) ||
           (mode == OhosOutputMode::kP3Sdr10 && p3_capable);
  };
  return ValidOhosOutputColor(from) && ValidOhosOutputColor(to) &&
         from.mode != to.mode && supported(from.mode) && supported(to.mode);
}
constexpr uint32_t OhosOutputNativeFormat(const OhosOutputColor& color) {
  return color.mode == OhosOutputMode::kSdr8 ? 12u : 34u;
}
}  // namespace ui
#endif
