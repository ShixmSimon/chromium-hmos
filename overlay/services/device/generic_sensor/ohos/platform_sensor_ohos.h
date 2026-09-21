// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_DEVICE_GENERIC_SENSOR_OHOS_PLATFORM_SENSOR_OHOS_H_
#define SERVICES_DEVICE_GENERIC_SENSOR_OHOS_PLATFORM_SENSOR_OHOS_H_

#include <sensors/oh_sensor_type.h>

#include <cstdint>
#include <memory>
#include <optional>

#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "services/device/generic_sensor/ohos/sensor_types_ohos.h"
#include "services/device/generic_sensor/platform_sensor.h"

namespace device {

namespace ohos {
class SensorSubscription;
}

class PlatformSensorOHOS : public PlatformSensor {
 public:
  static scoped_refptr<PlatformSensorOHOS> Create(
      mojom::SensorType type,
      SensorReadingSharedBuffer* reading_buffer,
      base::WeakPtr<PlatformSensorProvider> provider);

  static bool IsSupported(mojom::SensorType type);

  PlatformSensorOHOS(mojom::SensorType type,
                     SensorReadingSharedBuffer* reading_buffer,
                     base::WeakPtr<PlatformSensorProvider> provider);

  PlatformSensorOHOS(const PlatformSensorOHOS&) = delete;
  PlatformSensorOHOS& operator=(const PlatformSensorOHOS&) = delete;

  mojom::ReportingMode GetReportingMode() override;
  PlatformSensorConfiguration GetDefaultConfiguration() override;
  double GetMaximumSupportedFrequency() override;
  double GetMinimumSupportedFrequency() override;

 protected:
  ~PlatformSensorOHOS() override;

  bool StartSensor(const PlatformSensorConfiguration& configuration) override;
  void StopSensor() override;
  bool CheckSensorConfiguration(
      const PlatformSensorConfiguration& configuration) override;

 private:
  void NotifyPlatformSensorError();
  void UpdatePlatformSensorReading(int64_t timestamp_ns,
                                   double value1,
                                   double value2,
                                   double value3,
                                   double value4);

  std::optional<Sensor_Type> sensor_type_;
  std::optional<ohos::SensorCapabilities> capabilities_;
  std::unique_ptr<ohos::SensorSubscription> subscription_;
  int64_t current_sampling_interval_ns_ = 0;

  base::WeakPtrFactory<PlatformSensorOHOS> weak_factory_{this};
};

}  // namespace device

#endif  // SERVICES_DEVICE_GENERIC_SENSOR_OHOS_PLATFORM_SENSOR_OHOS_H_
