// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_DISPLAY_BRIGHTNESS_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_DISPLAY_BRIGHTNESS_H_
#include <algorithm>
#include <cmath>
#include <optional>
#include "ui/ozone/platform/ohos/ohos_output_color.h"
namespace ui {
// display.BrightnessInfo (API 22). Keep current headroom separate from the
// panel maximum: the latter must not override a thermal/power brightness cap.
struct OhosDisplayBrightness {
  uint64_t display_id = 0;
  OhosOutputColor hdr;
  double max_headroom = 1;
  bool operator==(const OhosDisplayBrightness& other) const {
    return display_id == other.display_id && hdr == other.hdr &&
           max_headroom == other.max_headroom;
  }
};
// NativeBuffer PQ uses a 203-nit content reference white. RenderService maps
// it to the panel's sdrNits itself; using physical panel nits here dims twice.
// Preserve current (possibly thermally limited) headroom in content units.
inline OhosOutputColor OhosContentOutputColor(
    const OhosDisplayBrightness& brightness) {
  constexpr uint32_t kReferenceWhiteMillinits = 203000;
  const double headroom = static_cast<double>(brightness.hdr.peak_millinits) /
                          brightness.hdr.white_millinits;
  return {OhosOutputMode::kHdr10, kReferenceWhiteMillinits,
          static_cast<uint32_t>(std::lround(std::min(
              kReferenceWhiteMillinits * headroom, 10000000.0)))};
}
inline std::optional<OhosDisplayBrightness> MakeOhosDisplayBrightness(
    uint64_t display_id,
    double sdr_nits,
    double current_headroom,
    double max_headroom) {
  if (!std::isfinite(sdr_nits) || !std::isfinite(current_headroom) ||
      !std::isfinite(max_headroom) || sdr_nits <= 0 || current_headroom <= 0 ||
      max_headroom <= 0)
    return std::nullopt;
  // PQ's representable range is 0..10000 nits. Headroom=1 is valid (including
  // temporary HDR dimming); do not switch formats as the brightness changes.
  const double white = std::clamp(sdr_nits, 0.001, 10000.0);
  const double headroom =
      std::max(1.0, std::min(current_headroom, max_headroom));
  return OhosDisplayBrightness{
      display_id,
      {OhosOutputMode::kHdr10, static_cast<uint32_t>(std::lround(white * 1000)),
       static_cast<uint32_t>(
           std::lround(std::min(white * headroom, 10000.0) * 1000))},
      max_headroom};
}
}  // namespace ui
#endif
