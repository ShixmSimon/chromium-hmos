// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/geolocation/ohos/geoposition_ohos.h"

#include "base/time/time.h"
#include "services/device/public/cpp/geolocation/geoposition.h"

namespace device {

namespace {

// LocationKit reports a heading in degrees and a speed in metres per second,
// and leaves both at an out-of-range value when the fix carries neither. The
// mojo struct has its own sentinels for "not reported", so each optional field
// is passed through a range check instead of being copied blind.
constexpr double kMinHeadingDegrees = 0.0;
constexpr double kMaxHeadingDegrees = 360.0;

double HeadingOrSentinel(double direction) {
  if (direction < kMinHeadingDegrees || direction > kMaxHeadingDegrees) {
    return mojom::kBadHeading;
  }
  return direction;
}

double SpeedOrSentinel(double speed) {
  return speed < 0.0 ? mojom::kBadSpeed : speed;
}

double AltitudeAccuracyOrSentinel(double altitude_accuracy) {
  return altitude_accuracy < 0.0 ? mojom::kBadAccuracy : altitude_accuracy;
}

// A satellite fix is the only source LocationKit describes as precise; the
// network and indoor sources are derived from nearby infrastructure and are
// what the coarse-location permission is meant to expose.
//
// It is also the only source that measures altitude, heading and speed. A
// Wi-Fi or cell fix carries no such instrument, and LocationKit fills those
// fields with zero rather than flagging them absent -- which a page reads as
// due north, stationary, at sea level. The sentinels say "not reported", and
// Chromium turns them into null.
bool HasInertialInstruments(Location_SourceType source) {
  return source == LOCATION_SOURCE_TYPE_GNSS ||
         source == LOCATION_SOURCE_TYPE_RTK;
}

mojom::GeopositionResultPtr MakeError(mojom::GeopositionErrorCode code,
                                      const char* message) {
  auto error = mojom::GeopositionError::New();
  error->error_code = code;
  error->error_message = message;
  return mojom::GeopositionResult::NewError(std::move(error));
}

}  // namespace

mojom::GeopositionResultPtr GeopositionResultFromBasicInfo(
    const Location_BasicInfo& info) {
  auto position = mojom::Geoposition::New();
  position->latitude = info.latitude;
  position->longitude = info.longitude;
  position->accuracy = info.accuracy;

  // Only pass the platform's own numbers through where it had something to
  // measure them with; elsewhere report them as absent rather than as zero.
  const bool measured = HasInertialInstruments(info.locationSourceType);
  position->altitude = measured ? info.altitude : mojom::kBadAltitude;
  position->altitude_accuracy =
      measured ? AltitudeAccuracyOrSentinel(info.altitudeAccuracy)
               : mojom::kBadAccuracy;
  position->heading =
      measured ? HeadingOrSentinel(info.direction) : mojom::kBadHeading;
  position->speed = measured ? SpeedOrSentinel(info.speed) : mojom::kBadSpeed;
  // LocationKit leaves timeForFix at zero for a fix it derived rather than
  // timed, and Chromium treats a null timestamp as an invalid position. The
  // arrival time is the honest answer there -- discarding the fix is not.
  position->timestamp =
      info.timeForFix > 0
          ? base::Time::FromMillisecondsSinceUnixEpoch(info.timeForFix)
          : base::Time::Now();
  position->is_precise = measured;

  if (!ValidateGeoposition(*position)) {
    return MakeError(mojom::GeopositionErrorCode::kPositionUnavailable,
                     "HarmonyOS reported a fix outside the valid range.");
  }
  return mojom::GeopositionResult::NewPosition(std::move(position));
}

mojom::GeopositionResultPtr GeopositionResultFromErrorCode(
    Location_ResultCode code) {
  switch (code) {
    case LOCATION_PERMISSION_DENIED:
      return MakeError(mojom::GeopositionErrorCode::kPermissionDenied,
                       "HarmonyOS denied the location permission.");
    case LOCATION_SWITCH_OFF:
      return MakeError(mojom::GeopositionErrorCode::kPositionUnavailable,
                       "The HarmonyOS location switch is off.");
    case LOCATION_NOT_SUPPORTED:
      return MakeError(mojom::GeopositionErrorCode::kPositionUnavailable,
                       "This HarmonyOS device has no location service.");
    default:
      return MakeError(mojom::GeopositionErrorCode::kPositionUnavailable,
                       "HarmonyOS could not determine a position.");
  }
}

}  // namespace device
