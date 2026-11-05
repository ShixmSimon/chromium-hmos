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

// RGB surface composition on some firmware ignores DelegateSrcRect. Place
// the entire allocation below a clipping parent instead: the parent's bounds
// are the visible destination, and this child rectangle preserves the exact
// source-to-destination mapping. This also works when source cropping *is*
// honored, since the submitted RGB source is always the full allocation.
// This mapping alone is NOT sufficient for promotion: the full allocation
// must also pass RgbVideoFitsPrimary before the primary hole is punched.
inline std::optional<OhosOverlayRect> RgbVideoChildGeometry(
    const OhosOverlayRect& destination,
    const OhosOverlayRect& source,
    float buffer_width,
    float buffer_height) {
  const OhosOverlayRect allocation{0, 0, buffer_width, buffer_height};
  if (!FiniteOverlayRect(destination) || !FiniteOverlayRect(source) ||
      !FiniteOverlayRect(allocation) || source.x < 0 || source.y < 0 ||
      source.x + source.width > buffer_width ||
      source.y + source.height > buffer_height)
    return std::nullopt;
  const double scale_x = double(destination.width) / source.width;
  const double scale_y = double(destination.height) / source.height;
  OhosOverlayRect child{float(-source.x * scale_x),
                        float(-source.y * scale_y),
                        float(buffer_width * scale_x),
                        float(buffer_height * scale_y)};
  if (!FiniteOverlayRect(child))
    return std::nullopt;
  return child;
}

// Retail RS may crop the RGB source at the display boundary, then stretch
// that source over the original node bounds. A clipping parent subsequently
// crops it again. Keep the entire transformed allocation within the webpage
// surface so this path cannot reach a display edge; otherwise use Chromium's
// normal compositor. Checking only the visible destination misses this bug.
inline bool RgbVideoFitsPrimary(const OhosOverlayRect& primary,
                                const OhosOverlayRect& destination,
                                const OhosOverlayRect& source,
                                float buffer_width,
                                float buffer_height) {
  const auto child = RgbVideoChildGeometry(destination, source, buffer_width,
                                          buffer_height);
  if (!FiniteOverlayRect(primary) || !child)
    return false;
  const double left = double(destination.x) + child->x;
  const double top = double(destination.y) + child->y;
  return left >= primary.x && top >= primary.y &&
         left + child->width <= double(primary.x) + primary.width &&
         top + child->height <= double(primary.y) + primary.height;
}

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
