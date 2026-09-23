// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/ohos_shape_detection/face_detection_ohos.h"

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

using shape_detection::mojom::FaceDetectionResult;
using shape_detection::mojom::FaceDetectionResultPtr;
using shape_detection::mojom::Landmark;
using shape_detection::mojom::LandmarkType;

std::optional<LandmarkType> LandmarkTypeFromName(const std::string* name) {
  if (!name) {
    return std::nullopt;
  }
  if (*name == "eye") {
    return LandmarkType::EYE;
  }
  if (*name == "nose") {
    return LandmarkType::NOSE;
  }
  if (*name == "mouth") {
    return LandmarkType::MOUTH;
  }
  return std::nullopt;
}

void AddLandmark(FaceDetectionResult& face,
                 LandmarkType type,
                 const gfx::PointF& location) {
  auto landmark = Landmark::New();
  landmark->type = type;
  landmark->locations.push_back(location);
  face.landmarks.push_back(std::move(landmark));
}

// Each eye and the nose become their own landmark. faceDetector reports the
// two corners of the mouth; they are averaged into one center point, as
// Android's backend and Huawei's ArkWeb report it, so pages that draw one
// marker per landmark keep working.
void AddLandmarks(FaceDetectionResult& face,
                  const base::ListValue* landmarks,
                  const DetectionImage& image) {
  if (!landmarks) {
    return;
  }
  float mouth_x = 0.0f;
  float mouth_y = 0.0f;
  int mouth_points = 0;
  for (const base::Value& value : *landmarks) {
    const base::DictValue* entry = value.GetIfDict();
    if (!entry) {
      continue;
    }
    std::optional<LandmarkType> type =
        LandmarkTypeFromName(entry->FindString("type"));
    std::optional<double> x = entry->FindDouble("x");
    std::optional<double> y = entry->FindDouble("y");
    if (!type || !x || !y) {
      continue;
    }
    const gfx::PointF location = image.ScaleBack(*x, *y);
    if (*type == LandmarkType::MOUTH) {
      mouth_x += location.x();
      mouth_y += location.y();
      ++mouth_points;
    } else {
      AddLandmark(face, *type, location);
    }
  }
  if (mouth_points > 0) {
    AddLandmark(face, LandmarkType::MOUTH,
                gfx::PointF(mouth_x / mouth_points, mouth_y / mouth_points));
  }
}

// One {left, top, width, height, landmarks} from ArkTS, or nullptr if it is
// malformed.
FaceDetectionResultPtr ToResult(const base::DictValue& entry,
                                const DetectionImage& image) {
  std::optional<double> left = entry.FindDouble("left");
  std::optional<double> top = entry.FindDouble("top");
  std::optional<double> width = entry.FindDouble("width");
  std::optional<double> height = entry.FindDouble("height");
  if (!left || !top || !width || !height) {
    return nullptr;
  }
  auto face = FaceDetectionResult::New();
  const gfx::PointF origin = image.ScaleBack(*left, *top);
  const gfx::PointF far_corner =
      image.ScaleBack(*left + *width, *top + *height);
  face->bounding_box =
      gfx::RectF(origin.x(), origin.y(), far_corner.x() - origin.x(),
                 far_corner.y() - origin.y());
  AddLandmarks(*face, entry.FindList("landmarks"), image);
  return face;
}

void OnDetected(uint32_t max_detected_faces,
                DetectionImage image,
                FaceDetectionOhos::DetectCallback callback,
                ohos_system_service::Reply reply) {
  std::vector<FaceDetectionResultPtr> results;
  if (!reply.ok) {
    LOG(WARNING) << "Face detection failed: " << reply.error;
    std::move(callback).Run(std::move(results));
    return;
  }
  const base::ListValue* faces = reply.result_dict().FindList("faces");
  if (faces) {
    for (const base::Value& value : *faces) {
      if (max_detected_faces > 0 && results.size() >= max_detected_faces) {
        break;
      }
      const base::DictValue* entry = value.GetIfDict();
      if (!entry) {
        continue;
      }
      FaceDetectionResultPtr face = ToResult(*entry, image);
      if (face) {
        results.push_back(std::move(face));
      }
    }
  }
  std::move(callback).Run(std::move(results));
}

}  // namespace

FaceDetectionOhos::FaceDetectionOhos(
    shape_detection::mojom::FaceDetectorOptionsPtr options)
    : max_detected_faces_(options ? options->max_detected_faces : 0) {}

FaceDetectionOhos::~FaceDetectionOhos() = default;

void FaceDetectionOhos::Detect(const SkBitmap& bitmap,
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
  ohos_system_service::Call(
      kShapeDetectionService, "face", std::move(args),
      base::BindOnce(&OnDetected, max_detected_faces_, std::move(*image),
                     std::move(callback)));
}

FaceDetectionProviderOhos::FaceDetectionProviderOhos() = default;
FaceDetectionProviderOhos::~FaceDetectionProviderOhos() = default;

// static
void FaceDetectionProviderOhos::Create(
    mojo::PendingReceiver<shape_detection::mojom::FaceDetectionProvider>
        receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<FaceDetectionProviderOhos>(),
                              std::move(receiver));
}

void FaceDetectionProviderOhos::CreateFaceDetection(
    mojo::PendingReceiver<shape_detection::mojom::FaceDetection> receiver,
    shape_detection::mojom::FaceDetectorOptionsPtr options) {
  mojo::MakeSelfOwnedReceiver(
      std::make_unique<FaceDetectionOhos>(std::move(options)),
      std::move(receiver));
}

}  // namespace ohos_shape_detection
