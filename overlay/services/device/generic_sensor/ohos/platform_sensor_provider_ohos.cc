// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/generic_sensor/ohos/platform_sensor_provider_ohos.h"

#include <memory>
#include <utility>

#include "services/device/generic_sensor/absolute_orientation_euler_angles_fusion_algorithm_using_accelerometer_and_magnetometer.h"
#include "services/device/generic_sensor/gravity_fusion_algorithm_using_accelerometer.h"
#include "services/device/generic_sensor/linear_acceleration_fusion_algorithm_using_accelerometer.h"
#include "services/device/generic_sensor/ohos/platform_sensor_ohos.h"
#include "services/device/generic_sensor/orientation_euler_angles_fusion_algorithm_using_quaternion.h"
#include "services/device/generic_sensor/orientation_quaternion_fusion_algorithm_using_euler_angles.h"
#include "services/device/generic_sensor/platform_sensor_fusion.h"

namespace device {

PlatformSensorProviderOHOS::PlatformSensorProviderOHOS() = default;
PlatformSensorProviderOHOS::~PlatformSensorProviderOHOS() = default;

base::WeakPtr<PlatformSensorProvider> PlatformSensorProviderOHOS::AsWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

void PlatformSensorProviderOHOS::CreateSensorInternal(
    mojom::SensorType type,
    CreateSensorCallback callback) {
  SensorReadingSharedBuffer* reading_buffer =
      GetSensorReadingSharedBufferForType(type);
  switch (type) {
    case mojom::SensorType::GRAVITY:
      CreateGravitySensor(reading_buffer, std::move(callback));
      return;
    case mojom::SensorType::LINEAR_ACCELERATION:
      CreateLinearAccelerationSensor(reading_buffer, std::move(callback));
      return;
    case mojom::SensorType::ABSOLUTE_ORIENTATION_EULER_ANGLES:
      CreateAbsoluteOrientationEulerAnglesSensor(std::move(callback));
      return;
    case mojom::SensorType::ABSOLUTE_ORIENTATION_QUATERNION:
      CreateAbsoluteOrientationQuaternionSensor(reading_buffer,
                                                std::move(callback));
      return;
    case mojom::SensorType::RELATIVE_ORIENTATION_EULER_ANGLES:
      CreateRelativeOrientationEulerAnglesSensor(std::move(callback));
      return;
    case mojom::SensorType::RELATIVE_ORIENTATION_QUATERNION:
      CreateRelativeOrientationQuaternionSensor(reading_buffer,
                                                std::move(callback));
      return;
    default:
      std::move(callback).Run(
          PlatformSensorOHOS::Create(type, reading_buffer, AsWeakPtr()));
  }
}

void PlatformSensorProviderOHOS::CreateGravitySensor(
    SensorReadingSharedBuffer* reading_buffer,
    CreateSensorCallback callback) {
  auto sensor = PlatformSensorOHOS::Create(mojom::SensorType::GRAVITY,
                                           reading_buffer, AsWeakPtr());
  if (sensor) {
    std::move(callback).Run(std::move(sensor));
    return;
  }

  PlatformSensorFusion::Create(
      AsWeakPtr(), std::make_unique<GravityFusionAlgorithmUsingAccelerometer>(),
      std::move(callback));
}

void PlatformSensorProviderOHOS::CreateLinearAccelerationSensor(
    SensorReadingSharedBuffer* reading_buffer,
    CreateSensorCallback callback) {
  auto sensor = PlatformSensorOHOS::Create(
      mojom::SensorType::LINEAR_ACCELERATION, reading_buffer, AsWeakPtr());
  if (sensor) {
    std::move(callback).Run(std::move(sensor));
    return;
  }

  PlatformSensorFusion::Create(
      AsWeakPtr(),
      std::make_unique<LinearAccelerationFusionAlgorithmUsingAccelerometer>(),
      std::move(callback));
}

void PlatformSensorProviderOHOS::CreateAbsoluteOrientationEulerAnglesSensor(
    CreateSensorCallback callback) {
  if (GetSensor(mojom::SensorType::ABSOLUTE_ORIENTATION_QUATERNION) ||
      PlatformSensorOHOS::IsSupported(
          mojom::SensorType::ABSOLUTE_ORIENTATION_QUATERNION)) {
    PlatformSensorFusion::Create(
        AsWeakPtr(),
        std::make_unique<OrientationEulerAnglesFusionAlgorithmUsingQuaternion>(
            true),
        std::move(callback));
    return;
  }

  PlatformSensorFusion::Create(
      AsWeakPtr(),
      std::make_unique<
          AbsoluteOrientationEulerAnglesFusionAlgorithmUsingAccelerometerAndMagnetometer>(),
      std::move(callback));
}

void PlatformSensorProviderOHOS::CreateAbsoluteOrientationQuaternionSensor(
    SensorReadingSharedBuffer* reading_buffer,
    CreateSensorCallback callback) {
  auto sensor = PlatformSensorOHOS::Create(
      mojom::SensorType::ABSOLUTE_ORIENTATION_QUATERNION, reading_buffer,
      AsWeakPtr());
  if (sensor) {
    std::move(callback).Run(std::move(sensor));
    return;
  }

  PlatformSensorFusion::Create(
      AsWeakPtr(),
      std::make_unique<OrientationQuaternionFusionAlgorithmUsingEulerAngles>(
          true),
      std::move(callback));
}

void PlatformSensorProviderOHOS::CreateRelativeOrientationEulerAnglesSensor(
    CreateSensorCallback callback) {
  if (!GetSensor(mojom::SensorType::RELATIVE_ORIENTATION_QUATERNION) &&
      !PlatformSensorOHOS::IsSupported(
          mojom::SensorType::RELATIVE_ORIENTATION_QUATERNION)) {
    std::move(callback).Run(nullptr);
    return;
  }

  PlatformSensorFusion::Create(
      AsWeakPtr(),
      std::make_unique<OrientationEulerAnglesFusionAlgorithmUsingQuaternion>(
          false),
      std::move(callback));
}

void PlatformSensorProviderOHOS::CreateRelativeOrientationQuaternionSensor(
    SensorReadingSharedBuffer* reading_buffer,
    CreateSensorCallback callback) {
  std::move(callback).Run(PlatformSensorOHOS::Create(
      mojom::SensorType::RELATIVE_ORIENTATION_QUATERNION, reading_buffer,
      AsWeakPtr()));
}

}  // namespace device
