// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/generic_sensor/ohos/sensor_types_ohos.h"

#include <sensors/oh_sensor.h>

#include <map>

#include "base/logging.h"
#include "base/no_destructor.h"

namespace device::ohos {

namespace {

// 200ms, the period the ArkWeb sensor adapter used.
constexpr double kDefaultSamplePeriodNs = 200000000.0;
constexpr double kNanosecondsPerSecond = 1000000000.0;

using CapabilitiesMap = std::map<Sensor_Type, SensorCapabilities>;

// Everything the platform reports, read once.
//
// OH_Sensor_GetInfos is a two-call API: the first asks how many sensors there
// are, the second fills an array of that size. Both the array and the count
// have to survive to OH_Sensor_DestroyInfos, so neither is reused.
CapabilitiesMap ReadCapabilities() {
  CapabilitiesMap capabilities;

  uint32_t count = 0;
  if (OH_Sensor_GetInfos(nullptr, &count) != SENSOR_SUCCESS || count == 0) {
    LOG(ERROR) << "OH_Sensor_GetInfos could not count the sensors";
    return capabilities;
  }

  Sensor_Info** infos = OH_Sensor_CreateInfos(count);
  if (!infos) {
    LOG(ERROR) << "OH_Sensor_CreateInfos returned null for " << count
               << " sensors";
    return capabilities;
  }
  if (OH_Sensor_GetInfos(infos, &count) != SENSOR_SUCCESS) {
    LOG(ERROR) << "OH_Sensor_GetInfos failed for " << count << " sensors";
    OH_Sensor_DestroyInfos(infos, count);
    return capabilities;
  }

  for (uint32_t i = 0; i < count; ++i) {
    Sensor_Type type;
    if (OH_SensorInfo_GetType(infos[i], &type) != SENSOR_SUCCESS) {
      continue;
    }
    SensorCapabilities entry;
    if (OH_SensorInfo_GetMinSamplingInterval(
            infos[i], &entry.min_sampling_interval_ns) != SENSOR_SUCCESS ||
        OH_SensorInfo_GetMaxSamplingInterval(
            infos[i], &entry.max_sampling_interval_ns) != SENSOR_SUCCESS) {
      // Present but unwilling to say at what rate. Leaving it out of the map
      // reports it as absent, which is the answer that keeps Chromium from
      // subscribing at a rate the sensor never agreed to.
      LOG(ERROR) << "Sensor " << type << " reports no sampling interval";
      continue;
    }
    capabilities[type] = entry;
  }

  OH_Sensor_DestroyInfos(infos, count);
  return capabilities;
}

const CapabilitiesMap& Capabilities() {
  static base::NoDestructor<CapabilitiesMap> capabilities(ReadCapabilities());
  return *capabilities;
}

}  // namespace

std::optional<Sensor_Type> ToOhosSensorType(mojom::SensorType type) {
  switch (type) {
    case mojom::SensorType::ACCELEROMETER:
      return SENSOR_TYPE_ACCELEROMETER;
    case mojom::SensorType::LINEAR_ACCELERATION:
      return SENSOR_TYPE_LINEAR_ACCELERATION;
    case mojom::SensorType::GRAVITY:
      return SENSOR_TYPE_GRAVITY;
    case mojom::SensorType::GYROSCOPE:
      return SENSOR_TYPE_GYROSCOPE;
    case mojom::SensorType::MAGNETOMETER:
      return SENSOR_TYPE_MAGNETIC_FIELD;
    case mojom::SensorType::ABSOLUTE_ORIENTATION_EULER_ANGLES:
      return SENSOR_TYPE_ORIENTATION;
    case mojom::SensorType::ABSOLUTE_ORIENTATION_QUATERNION:
      return SENSOR_TYPE_ROTATION_VECTOR;
    case mojom::SensorType::RELATIVE_ORIENTATION_QUATERNION:
      return SENSOR_TYPE_GAME_ROTATION_VECTOR;
    case mojom::SensorType::RELATIVE_ORIENTATION_EULER_ANGLES:
      // No OHOS sensor reports this directly. The provider fuses it from the
      // quaternion above, which is what the other platforms do too.
      return std::nullopt;
    case mojom::SensorType::AMBIENT_LIGHT:
      // SENSOR_TYPE_AMBIENT_LIGHT exists, but AmbientLightSensor stays off
      // until the reading has been through the rounding the spec requires.
      return std::nullopt;
  }

  LOG(ERROR) << "Unknown Chromium sensor type " << static_cast<int>(type);
  return std::nullopt;
}

std::optional<SensorCapabilities> GetSensorCapabilities(Sensor_Type type) {
  const CapabilitiesMap& capabilities = Capabilities();
  const auto it = capabilities.find(type);
  if (it == capabilities.end()) {
    return std::nullopt;
  }
  return it->second;
}

double DefaultSensorFrequencyHz() {
  return kNanosecondsPerSecond / kDefaultSamplePeriodNs;
}

}  // namespace device::ohos
