// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/generic_sensor/ohos/sensor_types_ohos.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace device::ohos {
namespace {

void ExpectMapping(mojom::SensorType chromium_type, Sensor_Type ohos_type) {
  const std::optional<Sensor_Type> mapped = ToOhosSensorType(chromium_type);
  ASSERT_TRUE(mapped.has_value());
  EXPECT_EQ(*mapped, ohos_type);
}

TEST(SensorTypesOhosTest, MapsDirectSensorsByEnumerator) {
  ExpectMapping(mojom::SensorType::ACCELEROMETER, SENSOR_TYPE_ACCELEROMETER);
  ExpectMapping(mojom::SensorType::LINEAR_ACCELERATION,
                SENSOR_TYPE_LINEAR_ACCELERATION);
  ExpectMapping(mojom::SensorType::GRAVITY, SENSOR_TYPE_GRAVITY);
  ExpectMapping(mojom::SensorType::GYROSCOPE, SENSOR_TYPE_GYROSCOPE);
  ExpectMapping(mojom::SensorType::MAGNETOMETER, SENSOR_TYPE_MAGNETIC_FIELD);
  ExpectMapping(mojom::SensorType::ABSOLUTE_ORIENTATION_EULER_ANGLES,
                SENSOR_TYPE_ORIENTATION);
  ExpectMapping(mojom::SensorType::ABSOLUTE_ORIENTATION_QUATERNION,
                SENSOR_TYPE_ROTATION_VECTOR);
  ExpectMapping(mojom::SensorType::RELATIVE_ORIENTATION_QUATERNION,
                SENSOR_TYPE_GAME_ROTATION_VECTOR);
}

TEST(SensorTypesOhosTest, LeavesFusedAndDisabledSensorsUnmapped) {
  EXPECT_FALSE(
      ToOhosSensorType(mojom::SensorType::RELATIVE_ORIENTATION_EULER_ANGLES)
          .has_value());
  EXPECT_FALSE(ToOhosSensorType(mojom::SensorType::AMBIENT_LIGHT).has_value());
}

TEST(SensorTypesOhosTest, RejectsUnknownChromiumSensorType) {
  EXPECT_FALSE(
      ToOhosSensorType(static_cast<mojom::SensorType>(100)).has_value());
}

TEST(SensorTypesOhosTest, UsesArkWebDefaultFrequency) {
  EXPECT_DOUBLE_EQ(DefaultSensorFrequencyHz(), 5.0);
}

}  // namespace
}  // namespace device::ohos
