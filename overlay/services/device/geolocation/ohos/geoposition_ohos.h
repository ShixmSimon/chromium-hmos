// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_DEVICE_GEOLOCATION_OHOS_GEOPOSITION_OHOS_H_
#define SERVICES_DEVICE_GEOLOCATION_OHOS_GEOPOSITION_OHOS_H_

#include <LocationKit/oh_location_type.h>

#include "services/device/public/mojom/geoposition.mojom.h"

namespace device {

// Translates one LocationKit fix into the geoposition the page observes.
//
// A fix whose latitude, longitude, accuracy or timestamp is outside the range
// Chromium considers valid becomes a kPositionUnavailable error rather than a
// position: LocationKit reports a fix even when the constellation has not
// converged, and forwarding that unchecked would hand the page coordinates it
// cannot distinguish from a real one.
//
// Pure -- it reads no NDK state, so it is exercisable off-device.
mojom::GeopositionResultPtr GeopositionResultFromBasicInfo(
    const Location_BasicInfo& info);

// Maps a LocationKit result code onto the error the page observes. Anything
// other than a denied permission surfaces as kPositionUnavailable, since the
// remaining codes describe a service that is absent or switched off rather
// than a decision the user made.
mojom::GeopositionResultPtr GeopositionResultFromErrorCode(
    Location_ResultCode code);

}  // namespace device

#endif  // SERVICES_DEVICE_GEOLOCATION_OHOS_GEOPOSITION_OHOS_H_
