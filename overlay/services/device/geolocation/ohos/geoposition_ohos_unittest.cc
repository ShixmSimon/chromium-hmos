// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/geolocation/ohos/geoposition_ohos.h"

#include "base/time/time.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace device {
namespace {

// Millisecond timestamp with no special meaning beyond being non-zero, so the
// converted position passes Chromium's "timestamp is set" check.
constexpr int64_t kFixTimeMs = 1'700'000'000'000;

Location_BasicInfo ValidFix() {
  Location_BasicInfo info = {};
  info.latitude = 39.9042;
  info.longitude = 116.4074;
  info.altitude = 43.5;
  info.accuracy = 12.0;
  info.speed = 1.5;
  info.direction = 90.0;
  info.timeForFix = kFixTimeMs;
  info.altitudeAccuracy = 3.0;
  info.locationSourceType = LOCATION_SOURCE_TYPE_GNSS;
  return info;
}

TEST(GeopositionOhosTest, ValidFixBecomesPosition) {
  const mojom::GeopositionResultPtr result =
      GeopositionResultFromBasicInfo(ValidFix());

  ASSERT_TRUE(result->is_position());
  const mojom::Geoposition& position = *result->get_position();
  EXPECT_DOUBLE_EQ(position.latitude, 39.9042);
  EXPECT_DOUBLE_EQ(position.longitude, 116.4074);
  EXPECT_DOUBLE_EQ(position.altitude, 43.5);
  EXPECT_DOUBLE_EQ(position.accuracy, 12.0);
  EXPECT_DOUBLE_EQ(position.altitude_accuracy, 3.0);
  EXPECT_DOUBLE_EQ(position.heading, 90.0);
  EXPECT_DOUBLE_EQ(position.speed, 1.5);
  EXPECT_EQ(position.timestamp,
            base::Time::FromMillisecondsSinceUnixEpoch(kFixTimeMs));
}

TEST(GeopositionOhosTest, SatelliteFixIsPrecise) {
  Location_BasicInfo info = ValidFix();
  info.locationSourceType = LOCATION_SOURCE_TYPE_GNSS;

  const mojom::GeopositionResultPtr result =
      GeopositionResultFromBasicInfo(info);

  ASSERT_TRUE(result->is_position());
  EXPECT_TRUE(result->get_position()->is_precise);
}

TEST(GeopositionOhosTest, NetworkFixIsNotPrecise) {
  Location_BasicInfo info = ValidFix();
  info.locationSourceType = LOCATION_SOURCE_TYPE_NETWORK;

  const mojom::GeopositionResultPtr result =
      GeopositionResultFromBasicInfo(info);

  ASSERT_TRUE(result->is_position());
  EXPECT_FALSE(result->get_position()->is_precise);
}

TEST(GeopositionOhosTest, NetworkFixReportsUnmeasuredFieldsAsAbsent) {
  Location_BasicInfo info = ValidFix();
  info.locationSourceType = LOCATION_SOURCE_TYPE_NETWORK;
  // What LocationKit actually fills in for a Wi-Fi fix: zeroes, not sentinels.
  info.altitude = 0.0;
  info.altitudeAccuracy = 0.0;
  info.direction = 0.0;
  info.speed = 0.0;

  const mojom::GeopositionResultPtr result =
      GeopositionResultFromBasicInfo(info);

  // Zero would read to the page as due north, stationary, at sea level.
  ASSERT_TRUE(result->is_position());
  const mojom::Geoposition& position = *result->get_position();
  EXPECT_DOUBLE_EQ(position.altitude, mojom::kBadAltitude);
  EXPECT_DOUBLE_EQ(position.altitude_accuracy, mojom::kBadAccuracy);
  EXPECT_DOUBLE_EQ(position.heading, mojom::kBadHeading);
  EXPECT_DOUBLE_EQ(position.speed, mojom::kBadSpeed);
  // The fix itself is still good.
  EXPECT_DOUBLE_EQ(position.latitude, 39.9042);
  EXPECT_DOUBLE_EQ(position.accuracy, 12.0);
}

TEST(GeopositionOhosTest, SatelliteFixKeepsItsMeasurements) {
  Location_BasicInfo info = ValidFix();
  info.locationSourceType = LOCATION_SOURCE_TYPE_GNSS;

  const mojom::GeopositionResultPtr result =
      GeopositionResultFromBasicInfo(info);

  ASSERT_TRUE(result->is_position());
  const mojom::Geoposition& position = *result->get_position();
  EXPECT_DOUBLE_EQ(position.altitude, 43.5);
  EXPECT_DOUBLE_EQ(position.altitude_accuracy, 3.0);
  EXPECT_DOUBLE_EQ(position.heading, 90.0);
  EXPECT_DOUBLE_EQ(position.speed, 1.5);
}

TEST(GeopositionOhosTest, OutOfRangeLatitudeBecomesError) {
  Location_BasicInfo info = ValidFix();
  info.latitude = 91.0;

  const mojom::GeopositionResultPtr result =
      GeopositionResultFromBasicInfo(info);

  ASSERT_TRUE(result->is_error());
  EXPECT_EQ(result->get_error()->error_code,
            mojom::GeopositionErrorCode::kPositionUnavailable);
}

TEST(GeopositionOhosTest, NegativeAccuracyBecomesError) {
  Location_BasicInfo info = ValidFix();
  info.accuracy = -1.0;

  const mojom::GeopositionResultPtr result =
      GeopositionResultFromBasicInfo(info);

  ASSERT_TRUE(result->is_error());
  EXPECT_EQ(result->get_error()->error_code,
            mojom::GeopositionErrorCode::kPositionUnavailable);
}

TEST(GeopositionOhosTest, UnsetTimestampFallsBackToArrivalTime) {
  Location_BasicInfo info = ValidFix();
  info.timeForFix = 0;
  const base::Time before = base::Time::Now();

  const mojom::GeopositionResultPtr result =
      GeopositionResultFromBasicInfo(info);

  // A fix LocationKit did not stamp is still a fix; Chromium only requires the
  // timestamp to be set, so it is filled in rather than discarding the fix.
  ASSERT_TRUE(result->is_position());
  const base::Time timestamp = result->get_position()->timestamp;
  EXPECT_FALSE(timestamp.is_null());
  EXPECT_GE(timestamp, before);
  EXPECT_LE(timestamp, base::Time::Now());
}

TEST(GeopositionOhosTest, OutOfRangeHeadingUsesSentinel) {
  Location_BasicInfo info = ValidFix();  // GNSS, so the field is passed through
  info.direction = -1.0;

  const mojom::GeopositionResultPtr result =
      GeopositionResultFromBasicInfo(info);

  ASSERT_TRUE(result->is_position());
  EXPECT_DOUBLE_EQ(result->get_position()->heading, mojom::kBadHeading);
}

TEST(GeopositionOhosTest, NegativeSpeedUsesSentinel) {
  Location_BasicInfo info = ValidFix();
  info.speed = -1.0;

  const mojom::GeopositionResultPtr result =
      GeopositionResultFromBasicInfo(info);

  ASSERT_TRUE(result->is_position());
  EXPECT_DOUBLE_EQ(result->get_position()->speed, mojom::kBadSpeed);
}

TEST(GeopositionOhosTest, DeniedPermissionKeepsItsOwnErrorCode) {
  const mojom::GeopositionResultPtr result =
      GeopositionResultFromErrorCode(LOCATION_PERMISSION_DENIED);

  ASSERT_TRUE(result->is_error());
  EXPECT_EQ(result->get_error()->error_code,
            mojom::GeopositionErrorCode::kPermissionDenied);
}

TEST(GeopositionOhosTest, SwitchOffIsUnavailableRatherThanDenied) {
  const mojom::GeopositionResultPtr result =
      GeopositionResultFromErrorCode(LOCATION_SWITCH_OFF);

  ASSERT_TRUE(result->is_error());
  EXPECT_EQ(result->get_error()->error_code,
            mojom::GeopositionErrorCode::kPositionUnavailable);
}

}  // namespace
}  // namespace device
