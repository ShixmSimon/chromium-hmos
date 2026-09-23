// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_OHOS_SHAPE_DETECTION_FACE_DETECTION_OHOS_H_
#define COMPONENTS_OHOS_SHAPE_DETECTION_FACE_DETECTION_OHOS_H_

#include <cstdint>

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "services/shape_detection/public/mojom/facedetection.mojom.h"
#include "services/shape_detection/public/mojom/facedetection_provider.mojom.h"

class SkBitmap;

namespace ohos_shape_detection {

// FaceDetection on CoreVisionKit's faceDetector, via ArkTS.
class FaceDetectionOhos : public shape_detection::mojom::FaceDetection {
 public:
  explicit FaceDetectionOhos(
      shape_detection::mojom::FaceDetectorOptionsPtr options);
  FaceDetectionOhos(const FaceDetectionOhos&) = delete;
  FaceDetectionOhos& operator=(const FaceDetectionOhos&) = delete;
  ~FaceDetectionOhos() override;

  // shape_detection::mojom::FaceDetection:
  void Detect(const SkBitmap& bitmap, DetectCallback callback) override;

 private:
  // 0 means no limit. `fast_mode` is not used: faceDetector has one mode.
  uint32_t max_detected_faces_ = 0;
};

class FaceDetectionProviderOhos
    : public shape_detection::mojom::FaceDetectionProvider {
 public:
  FaceDetectionProviderOhos();
  FaceDetectionProviderOhos(const FaceDetectionProviderOhos&) = delete;
  FaceDetectionProviderOhos& operator=(const FaceDetectionProviderOhos&) =
      delete;
  ~FaceDetectionProviderOhos() override;

  static void Create(
      mojo::PendingReceiver<shape_detection::mojom::FaceDetectionProvider>
          receiver);

  // shape_detection::mojom::FaceDetectionProvider:
  void CreateFaceDetection(
      mojo::PendingReceiver<shape_detection::mojom::FaceDetection> receiver,
      shape_detection::mojom::FaceDetectorOptionsPtr options) override;
};

}  // namespace ohos_shape_detection

#endif  // COMPONENTS_OHOS_SHAPE_DETECTION_FACE_DETECTION_OHOS_H_
