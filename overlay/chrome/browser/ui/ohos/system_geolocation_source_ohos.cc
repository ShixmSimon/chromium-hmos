// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/system_geolocation_source_ohos.h"

#include <utility>

#include "base/no_destructor.h"
#include "chrome/browser/permissions/system/system_permission_settings_ohos.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "services/device/public/cpp/geolocation/geolocation_system_permission_manager.h"

namespace chrome::ohos {

namespace {

// One state, read two ways. system_permission_settings holds it because
// PlatformHandle is built below this target and cannot reach up; this source
// translates it into the vocabulary the geolocation manager speaks.
device::LocationSystemPermissionStatus CurrentStatus() {
  switch (::system_permission_settings::GetOhosSystemPermission(
      ContentSettingsType::GEOLOCATION)) {
    case ::system_permission_settings::SystemPermission::kAllowed:
      return device::LocationSystemPermissionStatus::kAllowed;
    case ::system_permission_settings::SystemPermission::kDenied:
    case ::system_permission_settings::SystemPermission::kRestricted:
      return device::LocationSystemPermissionStatus::kDenied;
    case ::system_permission_settings::SystemPermission::kNotDetermined:
      return device::LocationSystemPermissionStatus::kNotDetermined;
  }
}

SystemGeolocationSourceOhos*& LiveSource() {
  static SystemGeolocationSourceOhos* source = nullptr;
  return source;
}

}  // namespace

SystemGeolocationSourceOhos::SystemGeolocationSourceOhos() {
  LiveSource() = this;
}

SystemGeolocationSourceOhos::~SystemGeolocationSourceOhos() {
  if (LiveSource() == this) {
    LiveSource() = nullptr;
  }
}

// static
std::unique_ptr<device::GeolocationSystemPermissionManager>
SystemGeolocationSourceOhos::CreateGeolocationSystemPermissionManager() {
  return std::make_unique<device::GeolocationSystemPermissionManager>(
      std::make_unique<SystemGeolocationSourceOhos>());
}

// static
void SystemGeolocationSourceOhos::NotifyPermissionChanged() {
  if (SystemGeolocationSourceOhos* source = LiveSource()) {
    source->NotifyCurrentStatus();
  }
}

void SystemGeolocationSourceOhos::RegisterPermissionUpdateCallback(
    PermissionUpdateCallback callback) {
  callback_ = std::move(callback);
  // The contract is that the callback receives the current value once on
  // registration, then again on every change.
  NotifyCurrentStatus();
}

void SystemGeolocationSourceOhos::NotifyCurrentStatus() {
  if (callback_) {
    callback_.Run(CurrentStatus());
  }
}

}  // namespace chrome::ohos
