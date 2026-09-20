// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_DEVICE_GEOLOCATION_OHOS_LOCATION_PROVIDER_OHOS_H_
#define SERVICES_DEVICE_GEOLOCATION_OHOS_LOCATION_PROVIDER_OHOS_H_

#include <LocationKit/oh_location_type.h>

#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "services/device/public/cpp/geolocation/location_provider.h"
#include "services/device/public/mojom/geolocation_internals.mojom.h"
#include "services/device/public/mojom/geoposition.mojom.h"

namespace device {

// LocationProvider backed by the HarmonyOS LocationKit NDK
// (`OH_Location_StartLocating` and friends).
//
// LocationKit delivers each fix on a thread of its own, while LocationProvider
// is a single-sequence interface. The provider therefore hands LocationKit a
// ref-counted relay rather than `this`, and the relay hops every fix back onto
// the sequence the provider was created on.
class LocationProviderOhos : public LocationProvider {
 public:
  LocationProviderOhos();

  LocationProviderOhos(const LocationProviderOhos&) = delete;
  LocationProviderOhos& operator=(const LocationProviderOhos&) = delete;

  ~LocationProviderOhos() override;

  // LocationProvider:
  void FillDiagnostics(mojom::GeolocationDiagnostics& diagnostics) override;
  void SetUpdateCallback(
      const LocationProviderUpdateCallback& callback) override;
  void StartProvider(bool high_accuracy) override;
  void StopProvider() override;
  const mojom::GeopositionResult* GetPosition() override;
  void OnPermissionGranted() override;

 private:
  // Keeps the C callback LocationKit holds decoupled from the provider's
  // lifetime. Defined in the .cc: nothing outside needs its shape.
  class FixRelay;

  // Opens a LocationKit session at the currently requested accuracy. Reports
  // an error result and leaves the provider stopped if that is not possible.
  void StartLocating();

  // Closes any open LocationKit session. Safe to call when none is open.
  void StopLocating();

  // Caches `result` and forwards it to the manager.
  void ReportResult(mojom::GeopositionResultPtr result);

  LocationProviderUpdateCallback update_callback_;
  mojom::GeopositionResultPtr last_result_;

  // Non-null exactly while a LocationKit session is open.
  raw_ptr<Location_RequestConfig> request_config_ = nullptr;
  scoped_refptr<FixRelay> relay_;

  // What the manager asked for, which outlives an individual session.
  bool start_requested_ = false;
  bool high_accuracy_ = false;

  // Until this is set, fixes are cached in `last_result_` but not published:
  // the interface forbids running the update callback before the grant.
  bool permission_granted_ = false;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<LocationProviderOhos> weak_factory_{this};
};

}  // namespace device

#endif  // SERVICES_DEVICE_GEOLOCATION_OHOS_LOCATION_PROVIDER_OHOS_H_
