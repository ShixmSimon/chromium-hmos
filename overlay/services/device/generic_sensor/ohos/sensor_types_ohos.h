// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_DEVICE_GENERIC_SENSOR_OHOS_SENSOR_TYPES_OHOS_H_
#define SERVICES_DEVICE_GENERIC_SENSOR_OHOS_SENSOR_TYPES_OHOS_H_

#include <sensors/oh_sensor_type.h>

#include <optional>

#include "services/device/public/mojom/sensor.mojom.h"

namespace device::ohos {

// The OHOS sensor a Chromium sensor type reads from, or nullopt when nothing
// on this platform answers for it.
//
// The mapping is written in terms of the enumerators rather than their values.
// An earlier port of this code stored the mojom side as bare integers, and
// mojom::SensorType has gained a member since: every sensor would have been
// wired to its neighbour, silently, because the numbers stayed valid.
std::optional<Sensor_Type> ToOhosSensorType(mojom::SensorType type);

// What the platform will accept for one sensor. The interval bounds are the
// ones OH_SensorInfo reports, in nanoseconds; frequency is their reciprocal,
// so the *minimum* interval is the *maximum* frequency.
struct SensorCapabilities {
  int64_t min_sampling_interval_ns = 0;
  int64_t max_sampling_interval_ns = 0;
};

// Absent when the platform does not list the sensor at all, which is how a
// device without it answers.
//
// Reads a snapshot of OH_Sensor_GetInfos taken once per process. Chromium asks
// these questions several times per configuration change -- supported, maximum
// frequency, minimum frequency, then the same again to validate -- and each
// answer would otherwise enumerate and free every sensor on the device. The
// cost of the snapshot is that a sensor appearing after the first call is not
// seen, which on a phone is not a thing that happens.
std::optional<SensorCapabilities> GetSensorCapabilities(Sensor_Type type);

// The default rate to report at when a client expresses no preference, in Hz.
// Matches what the ArkWeb sensor adapter used: a 200ms period.
double DefaultSensorFrequencyHz();

}  // namespace device::ohos

#endif  // SERVICES_DEVICE_GENERIC_SENSOR_OHOS_SENSOR_TYPES_OHOS_H_
