// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/geolocation/ohos/location_provider_ohos.h"

#include <LocationKit/oh_location.h>

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/synchronization/lock.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "components/device_event_log/device_event_log.h"
#include "services/device/geolocation/location_provider_manager.h"
#include "services/device/geolocation/ohos/geoposition_ohos.h"

namespace device {

namespace {

// LocationKit takes a reporting interval in whole seconds and treats it as the
// floor between fixes, not a deadline. One second matches what the Geolocation
// API's watchPosition callers expect while still letting the platform batch.
constexpr int kReportIntervalSeconds = 1;

}  // namespace

// Owns the callback LocationKit invokes. LocationKit is handed a raw pointer
// to this object, so it has to outlive every dispatch: the provider only drops
// its reference after `OH_Location_StopLocating` has returned.
class LocationProviderOhos::FixRelay
    : public base::RefCountedThreadSafe<FixRelay> {
 public:
  using Sink = base::RepeatingCallback<void(mojom::GeopositionResultPtr)>;

  explicit FixRelay(Sink sink) : sink_(std::move(sink)) {}

  FixRelay(const FixRelay&) = delete;
  FixRelay& operator=(const FixRelay&) = delete;

  // Invoked by LocationKit on one of its own threads.
  static void OnFix(Location_Info* info, void* user_data) {
    auto* relay = static_cast<FixRelay*>(user_data);
    if (!relay || !info) {
      return;
    }
    const Location_BasicInfo basic_info = OH_LocationInfo_GetBasicInfo(info);
    relay->Deliver(GeopositionResultFromBasicInfo(basic_info));
  }

  // Drops the sink so a fix already in flight is discarded rather than posted
  // to a sequence the provider may no longer be on.
  void Detach() {
    base::AutoLock auto_lock(lock_);
    sink_.Reset();
  }

 private:
  friend class base::RefCountedThreadSafe<FixRelay>;

  ~FixRelay() = default;

  void Deliver(mojom::GeopositionResultPtr result) {
    base::AutoLock auto_lock(lock_);
    if (sink_) {
      // `sink_` is bound through BindPostTask, so running it here only posts.
      sink_.Run(std::move(result));
    }
  }

  base::Lock lock_;
  Sink sink_ GUARDED_BY(lock_);
};

LocationProviderOhos::LocationProviderOhos() = default;

LocationProviderOhos::~LocationProviderOhos() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  StopLocating();
}

void LocationProviderOhos::FillDiagnostics(
    mojom::GeolocationDiagnostics& diagnostics) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!request_config_) {
    diagnostics.provider_state =
        mojom::GeolocationDiagnostics::ProviderState::kStopped;
    return;
  }
  diagnostics.provider_state =
      high_accuracy_ ? mojom::GeolocationDiagnostics::ProviderState::kHighAccuracy
                     : mojom::GeolocationDiagnostics::ProviderState::kLowAccuracy;
}

void LocationProviderOhos::SetUpdateCallback(
    const LocationProviderUpdateCallback& callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  update_callback_ = callback;
}

void LocationProviderOhos::StartProvider(bool high_accuracy) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // A session already running at the requested accuracy is left alone;
  // restarting it would discard the fix LocationKit has already converged on.
  if (start_requested_ && high_accuracy == high_accuracy_ && request_config_) {
    return;
  }
  start_requested_ = true;
  high_accuracy_ = high_accuracy;
  // Start straight away, as the Android and Linux providers do. Waiting for
  // OnPermissionGranted() here deadlocks: GeolocationProviderImpl only informs
  // providers of the grant once one is running, so a provider that refuses to
  // run until the grant never receives it. What the interface forbids before
  // the grant is running the update callback, and ReportResult() holds that
  // back instead.
  StartLocating();
}

void LocationProviderOhos::StopProvider() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  start_requested_ = false;
  StopLocating();
}

const mojom::GeopositionResult* LocationProviderOhos::GetPosition() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return last_result_.get();
}

void LocationProviderOhos::OnPermissionGranted() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (permission_granted_) {
    return;
  }
  permission_granted_ = true;
  // A fix that arrived before the grant was withheld; release it now so the
  // page does not wait out its timeout for a position already in hand.
  if (last_result_ && update_callback_) {
    update_callback_.Run(this, last_result_.Clone());
  }
}

void LocationProviderOhos::StartLocating() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  StopLocating();

  bool locating_enabled = false;
  const Location_ResultCode enabled_code =
      OH_Location_IsLocatingEnabled(&locating_enabled);
  if (enabled_code != LOCATION_SUCCESS) {
    GEOLOCATION_LOG(ERROR)
        << "OH_Location_IsLocatingEnabled failed: " << enabled_code;
    ReportResult(GeopositionResultFromErrorCode(enabled_code));
    return;
  }
  if (!locating_enabled) {
    ReportResult(GeopositionResultFromErrorCode(LOCATION_SWITCH_OFF));
    return;
  }

  request_config_ = OH_Location_CreateRequestConfig();
  if (!request_config_) {
    GEOLOCATION_LOG(ERROR) << "OH_Location_CreateRequestConfig returned null";
    ReportResult(GeopositionResultFromErrorCode(LOCATION_SERVICE_UNAVAILABLE));
    return;
  }

  OH_LocationRequestConfig_SetPowerConsumptionScene(
      request_config_, high_accuracy_ ? LOCATION_HIGH_POWER_CONSUMPTION
                                      : LOCATION_LOW_POWER_CONSUMPTION);
  OH_LocationRequestConfig_SetInterval(request_config_, kReportIntervalSeconds);

  relay_ = base::MakeRefCounted<FixRelay>(base::BindPostTask(
      base::SequencedTaskRunner::GetCurrentDefault(),
      base::BindRepeating(&LocationProviderOhos::ReportResult,
                          weak_factory_.GetWeakPtr())));
  OH_LocationRequestConfig_SetCallback(request_config_, &FixRelay::OnFix,
                                       relay_.get());

  const Location_ResultCode start_code =
      OH_Location_StartLocating(request_config_);
  if (start_code != LOCATION_SUCCESS) {
    GEOLOCATION_LOG(ERROR) << "OH_Location_StartLocating failed: " << start_code;
    StopLocating();
    ReportResult(GeopositionResultFromErrorCode(start_code));
  }
}

void LocationProviderOhos::StopLocating() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!request_config_) {
    return;
  }

  // Order matters. LocationKit holds a raw pointer to the relay and only stops
  // dispatching once StopLocating returns, so the reference is released after
  // that call rather than before it.
  const Location_ResultCode stop_code =
      OH_Location_StopLocating(request_config_);
  if (stop_code != LOCATION_SUCCESS) {
    GEOLOCATION_LOG(ERROR) << "OH_Location_StopLocating failed: " << stop_code;
  }
  OH_Location_DestroyRequestConfig(request_config_);
  request_config_ = nullptr;

  if (relay_) {
    relay_->Detach();
    relay_.reset();
  }
}

void LocationProviderOhos::ReportResult(mojom::GeopositionResultPtr result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  last_result_ = std::move(result);
  // Held back until the page has permission; OnPermissionGranted() releases it.
  if (permission_granted_ && update_callback_) {
    update_callback_.Run(this, last_result_.Clone());
  }
}

std::unique_ptr<LocationProvider> NewSystemLocationProvider() {
  return std::make_unique<LocationProviderOhos>();
}

}  // namespace device
