// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/ohos_shape_detection/shape_detection_service_ohos.h"

#include <utility>

#include "base/no_destructor.h"
#include "base/task/thread_pool.h"
#include "base/threading/sequence_bound.h"
#include "components/ohos_shape_detection/barcode_detection_ohos.h"
#include "components/ohos_shape_detection/face_detection_ohos.h"
#include "components/ohos_shape_detection/text_detection_ohos.h"

namespace ohos_shape_detection {

void BindShapeDetectionServiceInBrowser(
    mojo::PendingReceiver<shape_detection::mojom::ShapeDetectionService>
        receiver) {
  // One service for the life of the process, like the GPU-hosted one
  // upstream. Its sequence only needs to be sequenced: the ArkTS bridge
  // accepts calls from any sequence and replies on the caller's.
  static base::NoDestructor<base::SequenceBound<ShapeDetectionServiceOhos>>
      service(base::ThreadPool::CreateSequencedTaskRunner(
          {base::TaskPriority::USER_VISIBLE}));
  service->AsyncCall(&ShapeDetectionServiceOhos::AddReceiver)
      .WithArgs(std::move(receiver));
}

ShapeDetectionServiceOhos::ShapeDetectionServiceOhos() = default;
ShapeDetectionServiceOhos::~ShapeDetectionServiceOhos() = default;

void ShapeDetectionServiceOhos::AddReceiver(
    mojo::PendingReceiver<shape_detection::mojom::ShapeDetectionService>
        receiver) {
  receivers_.Add(this, std::move(receiver));
}

void ShapeDetectionServiceOhos::BindBarcodeDetectionProvider(
    mojo::PendingReceiver<shape_detection::mojom::BarcodeDetectionProvider>
        receiver) {
  BarcodeDetectionProviderOhos::Create(std::move(receiver));
}

void ShapeDetectionServiceOhos::BindFaceDetectionProvider(
    mojo::PendingReceiver<shape_detection::mojom::FaceDetectionProvider>
        receiver) {
  FaceDetectionProviderOhos::Create(std::move(receiver));
}

void ShapeDetectionServiceOhos::BindTextDetection(
    mojo::PendingReceiver<shape_detection::mojom::TextDetection> receiver) {
  TextDetectionOhos::Create(std::move(receiver));
}

}  // namespace ohos_shape_detection
