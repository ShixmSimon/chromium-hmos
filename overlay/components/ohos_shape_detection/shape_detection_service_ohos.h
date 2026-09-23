// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_OHOS_SHAPE_DETECTION_SHAPE_DETECTION_SERVICE_OHOS_H_
#define COMPONENTS_OHOS_SHAPE_DETECTION_SHAPE_DETECTION_SERVICE_OHOS_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "services/shape_detection/public/mojom/shape_detection_service.mojom.h"

namespace ohos_shape_detection {

// Binds `receiver` to the HarmonyOS shape detection service, hosted in this
// (the browser) process. Call it from the browser process only: the detectors
// reach ScanKit and CoreVisionKit through ArkTS, which no child process can
// reach. Callable from any sequence.
void BindShapeDetectionServiceInBrowser(
    mojo::PendingReceiver<shape_detection::mojom::ShapeDetectionService>
        receiver);

// Lives on a thread pool sequence, so the pixel copies, downscaling and NV21
// conversion that precede each detection stay off the UI thread.
class ShapeDetectionServiceOhos
    : public shape_detection::mojom::ShapeDetectionService {
 public:
  ShapeDetectionServiceOhos();
  ShapeDetectionServiceOhos(const ShapeDetectionServiceOhos&) = delete;
  ShapeDetectionServiceOhos& operator=(const ShapeDetectionServiceOhos&) =
      delete;
  ~ShapeDetectionServiceOhos() override;

  void AddReceiver(
      mojo::PendingReceiver<shape_detection::mojom::ShapeDetectionService>
          receiver);

  // shape_detection::mojom::ShapeDetectionService:
  void BindBarcodeDetectionProvider(
      mojo::PendingReceiver<shape_detection::mojom::BarcodeDetectionProvider>
          receiver) override;
  void BindFaceDetectionProvider(
      mojo::PendingReceiver<shape_detection::mojom::FaceDetectionProvider>
          receiver) override;
  void BindTextDetection(
      mojo::PendingReceiver<shape_detection::mojom::TextDetection> receiver)
      override;

 private:
  // A set rather than one receiver: content::GetShapeDetectionService() binds
  // a fresh pipe whenever its remote was reset after a disconnect.
  mojo::ReceiverSet<shape_detection::mojom::ShapeDetectionService> receivers_;
};

}  // namespace ohos_shape_detection

#endif  // COMPONENTS_OHOS_SHAPE_DETECTION_SHAPE_DETECTION_SERVICE_OHOS_H_
