// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_DEVICE_GENERIC_SENSOR_OHOS_PLATFORM_SENSOR_PROVIDER_OHOS_H_
#define SERVICES_DEVICE_GENERIC_SENSOR_OHOS_PLATFORM_SENSOR_PROVIDER_OHOS_H_

#include "base/memory/weak_ptr.h"
#include "services/device/generic_sensor/platform_sensor_provider.h"

namespace device {

class PlatformSensorProviderOHOS : public PlatformSensorProvider {
 public:
  PlatformSensorProviderOHOS();

  PlatformSensorProviderOHOS(const PlatformSensorProviderOHOS&) = delete;
  PlatformSensorProviderOHOS& operator=(const PlatformSensorProviderOHOS&) =
      delete;

  ~PlatformSensorProviderOHOS() override;

  base::WeakPtr<PlatformSensorProvider> AsWeakPtr() override;

 protected:
  void CreateSensorInternal(mojom::SensorType type,
                            CreateSensorCallback callback) override;

 private:
  void CreateGravitySensor(SensorReadingSharedBuffer* reading_buffer,
                           CreateSensorCallback callback);
  void CreateLinearAccelerationSensor(SensorReadingSharedBuffer* reading_buffer,
                                      CreateSensorCallback callback);
  void CreateAbsoluteOrientationEulerAnglesSensor(
      CreateSensorCallback callback);
  void CreateAbsoluteOrientationQuaternionSensor(
      SensorReadingSharedBuffer* reading_buffer,
      CreateSensorCallback callback);
  void CreateRelativeOrientationEulerAnglesSensor(
      CreateSensorCallback callback);
  void CreateRelativeOrientationQuaternionSensor(
      SensorReadingSharedBuffer* reading_buffer,
      CreateSensorCallback callback);

  base::WeakPtrFactory<PlatformSensorProviderOHOS> weak_factory_{this};
};

}  // namespace device

#endif  // SERVICES_DEVICE_GENERIC_SENSOR_OHOS_PLATFORM_SENSOR_PROVIDER_OHOS_H_
