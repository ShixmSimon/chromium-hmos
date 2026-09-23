// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/ohos_shape_detection/detection_image_ohos.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "skia/ext/image_operations.h"
#include "third_party/libyuv/include/libyuv/convert_from_argb.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImageInfo.h"

namespace ohos_shape_detection {

namespace {

constexpr size_t kBytesPerPixel = 4;

// Returns `bitmap` itself when it already fits within kMaxImageEdge, and a
// proportionally smaller copy otherwise, recording how to scale back.
// Returns an empty bitmap on failure.
SkBitmap FitWithinMaxEdge(const SkBitmap& bitmap,
                          float* scale_x,
                          float* scale_y) {
  *scale_x = 1.0f;
  *scale_y = 1.0f;
  const int width = bitmap.width();
  const int height = bitmap.height();
  const int longest = std::max(width, height);
  if (longest <= kMaxImageEdge) {
    return bitmap;
  }

  const double factor = static_cast<double>(kMaxImageEdge) / longest;
  const int scaled_width =
      std::max(1, static_cast<int>(std::lround(width * factor)));
  const int scaled_height =
      std::max(1, static_cast<int>(std::lround(height * factor)));
  // Resize() only takes kN32 bitmaps, which is what BitmapN32 guarantees;
  // it returns an empty bitmap for anything else.
  SkBitmap scaled = skia::ImageOperations::Resize(
      bitmap, skia::ImageOperations::RESIZE_GOOD, scaled_width, scaled_height);
  if (scaled.drawsNothing()) {
    LOG(WARNING) << "Could not downscale a " << width << "x" << height
                 << " image for shape detection";
    return SkBitmap();
  }
  *scale_x = static_cast<float>(width) / scaled_width;
  *scale_y = static_cast<float>(height) / scaled_height;
  return scaled;
}

// Reads `width` x `height` pixels from the top left of `bitmap` in the given
// layout, tightly packed.
std::optional<std::vector<uint8_t>> ReadPixels(const SkBitmap& bitmap,
                                               int width,
                                               int height,
                                               SkColorType color_type,
                                               SkAlphaType alpha_type) {
  const SkImageInfo info =
      SkImageInfo::Make(width, height, color_type, alpha_type);
  const size_t row_bytes = static_cast<size_t>(width) * kBytesPerPixel;
  std::vector<uint8_t> pixels(row_bytes * height);
  if (!bitmap.readPixels(info, pixels.data(), row_bytes, 0, 0)) {
    LOG(WARNING) << "Could not read the pixels of a shape detection image";
    return std::nullopt;
  }
  return pixels;
}

}  // namespace

DetectionImage::DetectionImage() = default;
DetectionImage::DetectionImage(DetectionImage&&) = default;
DetectionImage& DetectionImage::operator=(DetectionImage&&) = default;
DetectionImage::~DetectionImage() = default;

base::DictValue DetectionImage::TakeArgs(const char* pixels_key) {
  base::DictValue args;
  args.Set("width", width);
  args.Set("height", height);
  args.Set(pixels_key, std::move(pixels));
  return args;
}

gfx::PointF DetectionImage::ScaleBack(double x, double y) const {
  return gfx::PointF(static_cast<float>(x * scale_x),
                     static_cast<float>(y * scale_y));
}

std::optional<DetectionImage> PrepareRgbaImage(const SkBitmap& bitmap) {
  if (bitmap.drawsNothing()) {
    return std::nullopt;
  }
  DetectionImage image;
  SkBitmap fitted = FitWithinMaxEdge(bitmap, &image.scale_x, &image.scale_y);
  if (fitted.drawsNothing()) {
    return std::nullopt;
  }
  image.width = fitted.width();
  image.height = fitted.height();

  std::optional<std::vector<uint8_t>> rgba =
      ReadPixels(fitted, image.width, image.height, kRGBA_8888_SkColorType,
                 kUnpremul_SkAlphaType);
  if (!rgba) {
    return std::nullopt;
  }
  image.pixels = base::Base64Encode(*rgba);
  return image;
}

std::optional<DetectionImage> PrepareNv21Image(const SkBitmap& bitmap) {
  if (bitmap.drawsNothing()) {
    return std::nullopt;
  }
  DetectionImage image;
  SkBitmap fitted = FitWithinMaxEdge(bitmap, &image.scale_x, &image.scale_y);
  if (fitted.drawsNothing()) {
    return std::nullopt;
  }
  // Dropping the odd last row/column keeps the origin where it is, so the
  // scale factors above stay exact.
  image.width = fitted.width() & ~1;
  image.height = fitted.height() & ~1;
  if (image.width < 2 || image.height < 2) {
    return std::nullopt;
  }

  // libyuv's "ARGB" is B, G, R, A in memory, i.e. Skia's kBGRA_8888.
  // Premultiplied so that compositing over white is one addition per channel.
  std::optional<std::vector<uint8_t>> bgra =
      ReadPixels(fitted, image.width, image.height, kBGRA_8888_SkColorType,
                 kPremul_SkAlphaType);
  if (!bgra) {
    return std::nullopt;
  }
  base::span<uint8_t> pixels(*bgra);
  for (size_t i = 0; i + kBytesPerPixel <= pixels.size();
       i += kBytesPerPixel) {
    // Premultiplied color never exceeds alpha, so this cannot overflow.
    const int to_white = 255 - pixels[i + 3];
    pixels[i] = static_cast<uint8_t>(pixels[i] + to_white);
    pixels[i + 1] = static_cast<uint8_t>(pixels[i + 1] + to_white);
    pixels[i + 2] = static_cast<uint8_t>(pixels[i + 2] + to_white);
    pixels[i + 3] = 255;
  }

  const size_t y_size = static_cast<size_t>(image.width) * image.height;
  std::vector<uint8_t> nv21(y_size + y_size / 2);
  base::span<uint8_t> nv21_span(nv21);
  base::span<uint8_t> y_plane = nv21_span.first(y_size);
  base::span<uint8_t> vu_plane = nv21_span.subspan(y_size);
  const int bgra_stride = image.width * static_cast<int>(kBytesPerPixel);
  if (libyuv::ARGBToNV21(pixels.data(), bgra_stride, y_plane.data(),
                         image.width, vu_plane.data(), image.width,
                         image.width, image.height) != 0) {
    LOG(WARNING) << "Could not convert a shape detection image to NV21";
    return std::nullopt;
  }
  image.pixels = base::Base64Encode(nv21);
  return image;
}

std::vector<gfx::PointF> ReadPoints(const base::ListValue* list,
                                    const DetectionImage& image) {
  std::vector<gfx::PointF> points;
  if (!list) {
    return points;
  }
  for (const base::Value& entry : *list) {
    const base::DictValue* point = entry.GetIfDict();
    if (!point) {
      continue;
    }
    std::optional<double> x = point->FindDouble("x");
    std::optional<double> y = point->FindDouble("y");
    if (!x || !y) {
      continue;
    }
    points.push_back(image.ScaleBack(*x, *y));
  }
  return points;
}

}  // namespace ohos_shape_detection
