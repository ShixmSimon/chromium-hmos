// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_OHOS_SHAPE_DETECTION_BARCODE_DETECTION_OHOS_H_
#define COMPONENTS_OHOS_SHAPE_DETECTION_BARCODE_DETECTION_OHOS_H_

#include "base/containers/flat_set.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "services/shape_detection/public/mojom/barcodedetection.mojom.h"
#include "services/shape_detection/public/mojom/barcodedetection_provider.mojom.h"

class SkBitmap;

namespace ohos_shape_detection {

// BarcodeDetection on ScanKit's detectBarcode.decodeImage, via ArkTS.
class BarcodeDetectionOhos : public shape_detection::mojom::BarcodeDetection {
 public:
  explicit BarcodeDetectionOhos(
      shape_detection::mojom::BarcodeDetectorOptionsPtr options);
  BarcodeDetectionOhos(const BarcodeDetectionOhos&) = delete;
  BarcodeDetectionOhos& operator=(const BarcodeDetectionOhos&) = delete;
  ~BarcodeDetectionOhos() override;

  // shape_detection::mojom::BarcodeDetection:
  void Detect(const SkBitmap& bitmap, DetectCallback callback) override;

 private:
  // The formats the page asked for; empty means all of them.
  base::flat_set<shape_detection::mojom::BarcodeFormat> formats_;
};

class BarcodeDetectionProviderOhos
    : public shape_detection::mojom::BarcodeDetectionProvider {
 public:
  BarcodeDetectionProviderOhos();
  BarcodeDetectionProviderOhos(const BarcodeDetectionProviderOhos&) = delete;
  BarcodeDetectionProviderOhos& operator=(const BarcodeDetectionProviderOhos&) =
      delete;
  ~BarcodeDetectionProviderOhos() override;

  static void Create(
      mojo::PendingReceiver<shape_detection::mojom::BarcodeDetectionProvider>
          receiver);

  // shape_detection::mojom::BarcodeDetectionProvider:
  void CreateBarcodeDetection(
      mojo::PendingReceiver<shape_detection::mojom::BarcodeDetection> receiver,
      shape_detection::mojom::BarcodeDetectorOptionsPtr options) override;
  void EnumerateSupportedFormats(
      EnumerateSupportedFormatsCallback callback) override;
};

}  // namespace ohos_shape_detection

#endif  // COMPONENTS_OHOS_SHAPE_DETECTION_BARCODE_DETECTION_OHOS_H_
