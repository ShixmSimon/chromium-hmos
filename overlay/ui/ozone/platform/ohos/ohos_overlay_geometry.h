// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_GEOMETRY_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_GEOMETRY_H_

#include <algorithm>
#include <cmath>
#include <optional>
#include "ohos_overlay_bridge.h"

namespace ui::ohos_overlay {
inline bool FiniteOverlayRect(const OhosOverlayRect& r) {
  return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
         std::isfinite(r.height) && r.width > 0 && r.height > 0 &&
         std::abs(r.x) <= 16384 && std::abs(r.y) <= 16384 && r.width <= 16384 &&
         r.height <= 16384;
}

struct ClippedVideoGeometry {
  OhosOverlayRect destination;
  OhosOverlayRect source;
};

// RS accepts floating-point rectangles. Intersect the visible destination,
// preserving the source-to-destination mapping instead of rounding position or
// stretching a clipped video. Source may be normalized or in buffer pixels.
inline std::optional<ClippedVideoGeometry> ClipVideoGeometry(
    const OhosOverlayRect& primary,
    const OhosOverlayRect& destination,
    const OhosOverlayRect& source,
    const std::optional<OhosOverlayRect>& clip) {
  if (!FiniteOverlayRect(primary) || !FiniteOverlayRect(destination) ||
      !FiniteOverlayRect(source) || (clip && !FiniteOverlayRect(*clip)) ||
      primary.x < 0 || primary.y < 0 || source.x < 0 || source.y < 0 ||
      primary.x + primary.width > 16384 || primary.y + primary.height > 16384 ||
      source.x + source.width > 16384 || source.y + source.height > 16384)
    return std::nullopt;
  double left = std::max(primary.x, destination.x);
  double top = std::max(primary.y, destination.y);
  double right = std::min(double(primary.x) + primary.width,
                          double(destination.x) + destination.width);
  double bottom = std::min(double(primary.y) + primary.height,
                           double(destination.y) + destination.height);
  if (clip) {
    left = std::max(left, double(clip->x));
    top = std::max(top, double(clip->y));
    right = std::min(right, double(clip->x) + clip->width);
    bottom = std::min(bottom, double(clip->y) + clip->height);
  }
  if (left >= right || top >= bottom)
    return std::nullopt;
  const double scale_x = double(source.width) / destination.width;
  const double scale_y = double(source.height) / destination.height;
  ClippedVideoGeometry result{
      {float(left), float(top), float(right - left), float(bottom - top)},
      {float(source.x + (left - destination.x) * scale_x),
       float(source.y + (top - destination.y) * scale_y),
       float((right - left) * scale_x), float((bottom - top) * scale_y)}};
  // Keep floating-point rounding inside the original source allocation.
  result.source.width =
      std::min(result.source.width, source.x + source.width - result.source.x);
  result.source.height = std::min(result.source.height,
                                  source.y + source.height - result.source.y);
  if (!FiniteOverlayRect(result.destination) ||
      !FiniteOverlayRect(result.source))
    return std::nullopt;
  return result;
}
}  // namespace ui::ohos_overlay
#endif
