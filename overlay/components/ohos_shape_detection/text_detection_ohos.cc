// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/ohos_shape_detection/text_detection_ohos.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "components/ohos_shape_detection/detection_image_ohos.h"
#include "components/ohos_system_service/system_service_ohos.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect_f.h"

namespace ohos_shape_detection {

namespace {

using shape_detection::mojom::TextDetectionResult;
using shape_detection::mojom::TextDetectionResultPtr;

// The smallest axis-aligned box around `points`; empty for no points.
gfx::RectF BoundsOf(const std::vector<gfx::PointF>& points) {
  if (points.empty()) {
    return gfx::RectF();
  }
  gfx::PointF min = points.front();
  gfx::PointF max = points.front();
  for (const gfx::PointF& point : points) {
    min.SetToMin(point);
    max.SetToMax(point);
  }
  return gfx::BoundingRect(min, max);
}

void OnDetected(DetectionImage image,
                TextDetectionOhos::DetectCallback callback,
                ohos_system_service::Reply reply) {
  std::vector<TextDetectionResultPtr> results;
  if (!reply.ok) {
    LOG(WARNING) << "Text detection failed: " << reply.error;
    std::move(callback).Run(std::move(results));
    return;
  }
  const base::ListValue* texts = reply.result_dict().FindList("texts");
  if (texts) {
    for (const base::Value& value : *texts) {
      const base::DictValue* entry = value.GetIfDict();
      if (!entry) {
        continue;
      }
      const std::string* raw_value = entry->FindString("rawValue");
      if (!raw_value) {
        continue;
      }
      auto result = TextDetectionResult::New();
      result->raw_value = *raw_value;
      // textRecognition gives each line's corners clockwise from the top
      // left, which is the order the spec asks for; the box is derived from
      // them because rotated lines have no separate rectangle.
      result->corner_points = ReadPoints(entry->FindList("corners"), image);
      result->bounding_box = BoundsOf(result->corner_points);
      results.push_back(std::move(result));
    }
  }
  std::move(callback).Run(std::move(results));
}

}  // namespace

TextDetectionOhos::TextDetectionOhos() = default;
TextDetectionOhos::~TextDetectionOhos() = default;

// static
void TextDetectionOhos::Create(
    mojo::PendingReceiver<shape_detection::mojom::TextDetection> receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<TextDetectionOhos>(),
                              std::move(receiver));
}

void TextDetectionOhos::Detect(const SkBitmap& bitmap,
                               DetectCallback callback) {
  std::optional<DetectionImage> image = PrepareRgbaImage(bitmap);
  if (!image) {
    std::move(callback).Run({});
    return;
  }
  // Built before `image` is moved into the reply callback; argument
  // evaluation order is unspecified.
  base::DictValue args = image->TakeArgs("rgba");
  // No pointer to `this`: the bridge answers every call exactly once, and a
  // reply for a detector the page has dropped is discarded by mojo.
  ohos_system_service::Call(kShapeDetectionService, "text", std::move(args),
                            base::BindOnce(&OnDetected, std::move(*image),
                                           std::move(callback)));
}

}  // namespace ohos_shape_detection
