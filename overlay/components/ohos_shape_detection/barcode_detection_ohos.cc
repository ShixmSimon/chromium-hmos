// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/ohos_shape_detection/barcode_detection_ohos.h"

#include <array>
#include <memory>
#include <optional>
#include <string_view>
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

using shape_detection::mojom::BarcodeDetectionResult;
using shape_detection::mojom::BarcodeDetectionResultPtr;
using shape_detection::mojom::BarcodeFormat;

struct FormatName {
  std::string_view name;
  BarcodeFormat format;
};

// Everything ScanKit recognizes, by the Web BarcodeFormat names the ArkTS
// side already translates ScanKit's scanType into.
constexpr auto kFormats = std::to_array<FormatName>({
    {"aztec", BarcodeFormat::AZTEC},
    {"codabar", BarcodeFormat::CODABAR},
    {"code_39", BarcodeFormat::CODE_39},
    {"code_93", BarcodeFormat::CODE_93},
    {"code_128", BarcodeFormat::CODE_128},
    {"data_matrix", BarcodeFormat::DATA_MATRIX},
    {"ean_8", BarcodeFormat::EAN_8},
    {"ean_13", BarcodeFormat::EAN_13},
    {"itf", BarcodeFormat::ITF},
    {"pdf417", BarcodeFormat::PDF417},
    {"qr_code", BarcodeFormat::QR_CODE},
    {"upc_a", BarcodeFormat::UPC_A},
    {"upc_e", BarcodeFormat::UPC_E},
});

BarcodeFormat FormatFromName(const std::string* name) {
  if (name) {
    for (const FormatName& entry : kFormats) {
      if (entry.name == *name) {
        return entry.format;
      }
    }
  }
  return BarcodeFormat::UNKNOWN;
}

// One {rawValue, format, left, top, right, bottom, corners?} from ArkTS, or
// nullptr if it is malformed.
BarcodeDetectionResultPtr ToResult(const base::DictValue& entry,
                                   const DetectionImage& image) {
  const std::string* raw_value = entry.FindString("rawValue");
  std::optional<double> left = entry.FindDouble("left");
  std::optional<double> top = entry.FindDouble("top");
  std::optional<double> right = entry.FindDouble("right");
  std::optional<double> bottom = entry.FindDouble("bottom");
  if (!raw_value || !left || !top || !right || !bottom) {
    return nullptr;
  }

  auto result = BarcodeDetectionResult::New();
  result->raw_value = *raw_value;
  result->format = FormatFromName(entry.FindString("format"));

  const gfx::PointF top_left = image.ScaleBack(*left, *top);
  const gfx::PointF bottom_right = image.ScaleBack(*right, *bottom);
  result->bounding_box =
      gfx::RectF(top_left.x(), top_left.y(), bottom_right.x() - top_left.x(),
                 bottom_right.y() - top_left.y());

  result->corner_points = ReadPoints(entry.FindList("corners"), image);
  if (result->corner_points.size() != 4) {
    // ScanKit only reports corners on newer releases. The spec wants four,
    // clockwise from the top left; the bounding box's are the best fallback.
    const gfx::RectF& box = result->bounding_box;
    result->corner_points = {box.origin(), box.top_right(),
                             box.bottom_right(), box.bottom_left()};
  }
  return result;
}

void OnDetected(base::flat_set<BarcodeFormat> formats,
                DetectionImage image,
                BarcodeDetectionOhos::DetectCallback callback,
                ohos_system_service::Reply reply) {
  std::vector<BarcodeDetectionResultPtr> results;
  if (!reply.ok) {
    LOG(WARNING) << "Barcode detection failed: " << reply.error;
    std::move(callback).Run(std::move(results));
    return;
  }
  const base::ListValue* found = reply.result_dict().FindList("results");
  if (found) {
    for (const base::Value& value : *found) {
      const base::DictValue* entry = value.GetIfDict();
      if (!entry) {
        continue;
      }
      BarcodeDetectionResultPtr result = ToResult(*entry, image);
      if (!result) {
        continue;
      }
      // ScanKit is always asked for every symbology, so the page's filter
      // is applied here. A filtered request never gets UNKNOWN codes.
      if (!formats.empty() && !formats.contains(result->format)) {
        continue;
      }
      results.push_back(std::move(result));
    }
  }
  std::move(callback).Run(std::move(results));
}

}  // namespace

BarcodeDetectionOhos::BarcodeDetectionOhos(
    shape_detection::mojom::BarcodeDetectorOptionsPtr options) {
  if (options) {
    for (BarcodeFormat format : options->formats) {
      // Blink never sends UNKNOWN as a hint; ignoring it keeps a bogus hint
      // from filtering out every result.
      if (format != BarcodeFormat::UNKNOWN) {
        formats_.insert(format);
      }
    }
  }
}

BarcodeDetectionOhos::~BarcodeDetectionOhos() = default;

void BarcodeDetectionOhos::Detect(const SkBitmap& bitmap,
                                  DetectCallback callback) {
  std::optional<DetectionImage> image = PrepareNv21Image(bitmap);
  if (!image) {
    std::move(callback).Run({});
    return;
  }
  // Built before `image` is moved into the reply callback; argument
  // evaluation order is unspecified.
  base::DictValue args = image->TakeArgs("nv21");
  // The callback holds no pointer to `this`: the bridge answers every call
  // exactly once, and if the page drops the detector meanwhile, mojo just
  // discards the reply.
  ohos_system_service::Call(
      kShapeDetectionService, "barcode", std::move(args),
      base::BindOnce(&OnDetected, formats_, std::move(*image),
                     std::move(callback)));
}

BarcodeDetectionProviderOhos::BarcodeDetectionProviderOhos() = default;
BarcodeDetectionProviderOhos::~BarcodeDetectionProviderOhos() = default;

// static
void BarcodeDetectionProviderOhos::Create(
    mojo::PendingReceiver<shape_detection::mojom::BarcodeDetectionProvider>
        receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<BarcodeDetectionProviderOhos>(),
                              std::move(receiver));
}

void BarcodeDetectionProviderOhos::CreateBarcodeDetection(
    mojo::PendingReceiver<shape_detection::mojom::BarcodeDetection> receiver,
    shape_detection::mojom::BarcodeDetectorOptionsPtr options) {
  mojo::MakeSelfOwnedReceiver(
      std::make_unique<BarcodeDetectionOhos>(std::move(options)),
      std::move(receiver));
}

void BarcodeDetectionProviderOhos::EnumerateSupportedFormats(
    EnumerateSupportedFormatsCallback callback) {
  // Static: ScanKit's symbologies do not depend on the device, and asking
  // ArkTS would make BarcodeDetector.getSupportedFormats() fail whenever the
  // bridge is not attached yet.
  std::vector<BarcodeFormat> formats;
  formats.reserve(kFormats.size());
  for (const FormatName& entry : kFormats) {
    formats.push_back(entry.format);
  }
  std::move(callback).Run(std::move(formats));
}

}  // namespace ohos_shape_detection
