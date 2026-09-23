// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_OHOS_SHAPE_DETECTION_TEXT_DETECTION_OHOS_H_
#define COMPONENTS_OHOS_SHAPE_DETECTION_TEXT_DETECTION_OHOS_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "services/shape_detection/public/mojom/textdetection.mojom.h"

class SkBitmap;

namespace ohos_shape_detection {

// TextDetection on CoreVisionKit's textRecognition, via ArkTS. One result
// per recognized line.
class TextDetectionOhos : public shape_detection::mojom::TextDetection {
 public:
  TextDetectionOhos();
  TextDetectionOhos(const TextDetectionOhos&) = delete;
  TextDetectionOhos& operator=(const TextDetectionOhos&) = delete;
  ~TextDetectionOhos() override;

  static void Create(
      mojo::PendingReceiver<shape_detection::mojom::TextDetection> receiver);

  // shape_detection::mojom::TextDetection:
  void Detect(const SkBitmap& bitmap, DetectCallback callback) override;
};

}  // namespace ohos_shape_detection

#endif  // COMPONENTS_OHOS_SHAPE_DETECTION_TEXT_DETECTION_OHOS_H_
