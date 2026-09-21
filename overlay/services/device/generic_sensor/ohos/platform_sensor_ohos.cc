// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/generic_sensor/ohos/platform_sensor_ohos.h"

#include <cmath>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/bind_post_task.h"
#include "services/device/generic_sensor/ohos/sensor_subscription_ohos.h"
#include "services/device/public/cpp/generic_sensor/platform_sensor_configuration.h"

namespace device {

namespace {

constexpr double kNanosecondsPerSecond = 1'000'000'000.0;

double FrequencyFromInterval(int64_t interval_ns) {
  if (interval_ns <= 0) {
    return 0.0;
  }
  return kNanosecondsPerSecond / static_cast<double>(interval_ns);
}

}  // namespace

// static
scoped_refptr<PlatformSensorOHOS> PlatformSensorOHOS::Create(
    mojom::SensorType type,
    SensorReadingSharedBuffer* reading_buffer,
    base::WeakPtr<PlatformSensorProvider> provider) {
  auto sensor = base::MakeRefCounted<PlatformSensorOHOS>(type, reading_buffer,
                                                         std::move(provider));
  if (!sensor->sensor_type_ || !sensor->capabilities_) {
    return nullptr;
  }
  return sensor;
}

// static
bool PlatformSensorOHOS::IsSupported(mojom::SensorType type) {
  const std::optional<Sensor_Type> sensor_type = ohos::ToOhosSensorType(type);
  return sensor_type && ohos::GetSensorCapabilities(*sensor_type).has_value();
}

PlatformSensorOHOS::PlatformSensorOHOS(
    mojom::SensorType type,
    SensorReadingSharedBuffer* reading_buffer,
    base::WeakPtr<PlatformSensorProvider> provider)
    : PlatformSensor(type, reading_buffer, std::move(provider)),
      sensor_type_(ohos::ToOhosSensorType(type)) {
  if (sensor_type_) {
    capabilities_ = ohos::GetSensorCapabilities(*sensor_type_);
  }
}

PlatformSensorOHOS::~PlatformSensorOHOS() {
  weak_factory_.InvalidateWeakPtrs();
  StopSensor();
}

mojom::ReportingMode PlatformSensorOHOS::GetReportingMode() {
  return mojom::ReportingMode::CONTINUOUS;
}

PlatformSensorConfiguration PlatformSensorOHOS::GetDefaultConfiguration() {
  return PlatformSensorConfiguration(ohos::DefaultSensorFrequencyHz());
}

double PlatformSensorOHOS::GetMaximumSupportedFrequency() {
  return capabilities_
             ? FrequencyFromInterval(capabilities_->min_sampling_interval_ns)
             : 0.0;
}

double PlatformSensorOHOS::GetMinimumSupportedFrequency() {
  return capabilities_
             ? FrequencyFromInterval(capabilities_->max_sampling_interval_ns)
             : 0.0;
}

bool PlatformSensorOHOS::StartSensor(
    const PlatformSensorConfiguration& configuration) {
  if (!sensor_type_ || !CheckSensorConfiguration(configuration)) {
    LOG(ERROR) << "Invalid configuration for OHOS sensor " << GetType() << ": "
               << configuration.frequency() << " Hz";
    return false;
  }

  const int64_t sampling_interval_ns =
      static_cast<int64_t>(kNanosecondsPerSecond / configuration.frequency());
  if (subscription_ && current_sampling_interval_ns_ == sampling_interval_ns) {
    return true;
  }

  StopSensor();
  auto callback = base::BindPostTask(
      main_task_runner(),
      base::BindRepeating(&PlatformSensorOHOS::UpdatePlatformSensorReading,
                          weak_factory_.GetWeakPtr()));
  subscription_ = ohos::SensorSubscription::Create(
      *sensor_type_, sampling_interval_ns, std::move(callback));
  if (!subscription_) {
    LOG(ERROR) << "Could not subscribe to OHOS sensor " << GetType();
    NotifyPlatformSensorError();
    return false;
  }

  current_sampling_interval_ns_ = sampling_interval_ns;
  return true;
}

void PlatformSensorOHOS::StopSensor() {
  subscription_.reset();
  current_sampling_interval_ns_ = 0;
}

bool PlatformSensorOHOS::CheckSensorConfiguration(
    const PlatformSensorConfiguration& configuration) {
  const double frequency = configuration.frequency();
  const double minimum = GetMinimumSupportedFrequency();
  const double maximum = GetMaximumSupportedFrequency();
  return std::isfinite(frequency) && minimum > 0.0 && maximum > 0.0 &&
         frequency >= minimum && frequency <= maximum;
}

void PlatformSensorOHOS::NotifyPlatformSensorError() {
  PostTaskToMainSequence(FROM_HERE,
                         base::BindOnce(&PlatformSensorOHOS::NotifySensorError,
                                        weak_factory_.GetWeakPtr()));
}

void PlatformSensorOHOS::UpdatePlatformSensorReading(int64_t timestamp_ns,
                                                     double value1,
                                                     double value2,
                                                     double value3,
                                                     double value4) {
  SensorReading reading;
  reading.raw.timestamp =
      static_cast<double>(timestamp_ns) / kNanosecondsPerSecond;
  reading.raw.values[0] = value1;
  reading.raw.values[1] = value2;
  reading.raw.values[2] = value3;
  reading.raw.values[3] = value4;
  UpdateSharedBufferAndNotifyClients(reading);
}

}  // namespace device
