// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_OHOS_SHAPE_DETECTION_DETECTION_IMAGE_OHOS_H_
#define COMPONENTS_OHOS_SHAPE_DETECTION_DETECTION_IMAGE_OHOS_H_

#include <optional>
#include <string>
#include <vector>

#include "base/values.h"
#include "ui/gfx/geometry/point_f.h"

class SkBitmap;

namespace ohos_shape_detection {

// The name the ArkTS ShapeDetectionService answers to on the system service
// bridge (overlay/chromium-ui/engine/src/main/ets/utils/SystemServices.ets).
inline constexpr char kShapeDetectionService[] = "shape";

// The longest edge sent to ArkTS. Pixels cross the bridge as base64 inside
// JSON, so a 4K frame would be ~44 MB of text; the kits do not need more than
// this to find codes, faces or lines of text.
inline constexpr int kMaxImageEdge = 1920;

// An image ready to be sent to ArkTS, plus what it takes to map the kit's
// coordinates back onto the bitmap the page passed in.
struct DetectionImage {
  DetectionImage();
  DetectionImage(DetectionImage&&);
  DetectionImage& operator=(DetectionImage&&);
  ~DetectionImage();

  // The size of the image sent.
  int width = 0;
  int height = 0;

  // Multiply a coordinate in the image sent by these to get one in the
  // page's bitmap.
  float scale_x = 1.0f;
  float scale_y = 1.0f;

  // The pixels, base64-encoded.
  std::string pixels;

  // The request arguments ArkTS expects: {width, height, <pixels_key>}. Moves
  // `pixels` out rather than copying what can be several megabytes.
  base::DictValue TakeArgs(const char* pixels_key);

  // Maps a point in the image sent onto the page's bitmap.
  gfx::PointF ScaleBack(double x, double y) const;
};

// Tightly packed, unpremultiplied RGBA_8888, which is what the ArkTS side
// declares as the PixelMap's srcPixelFormat. Used by face and text detection.
// Returns nullopt for an empty or unreadable bitmap.
std::optional<DetectionImage> PrepareRgbaImage(const SkBitmap& bitmap);

// NV21 (full-size Y plane followed by interleaved V/U at quarter size), the
// only raw format ScanKit's detectBarcode.decodeImage accepts. NV21 needs even
// dimensions, so an odd last row or column is dropped; that never moves the
// coordinates of what is found. Transparent pixels are composited over white
// first, because a barcode drawn on a transparent canvas would otherwise turn
// into black on black. Returns nullopt for an image smaller than 2x2.
std::optional<DetectionImage> PrepareNv21Image(const SkBitmap& bitmap);

// Reads a list of {x, y} dicts, scaled back into the page's bitmap. Entries
// that are not dicts with numeric x and y are skipped.
std::vector<gfx::PointF> ReadPoints(const base::ListValue* list,
                                    const DetectionImage& image);

}  // namespace ohos_shape_detection

#endif  // COMPONENTS_OHOS_SHAPE_DETECTION_DETECTION_IMAGE_OHOS_H_
