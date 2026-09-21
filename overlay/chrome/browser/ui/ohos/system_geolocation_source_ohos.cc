// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/system_geolocation_source_ohos.h"

#include <utility>

#include "base/memory/ptr_util.h"
#include "base/no_destructor.h"
#include "services/device/public/cpp/geolocation/geolocation_system_permission_manager.h"

namespace chrome::ohos {

namespace {

// The shell can report before the source is constructed, and the source can be
// constructed before the shell has looked. Neither ordering is under our
// control, so the last reported value lives here and whichever arrives second
// picks it up.
device::LocationSystemPermissionStatus& CachedStatus() {
  static device::LocationSystemPermissionStatus status =
      device::LocationSystemPermissionStatus::kNotDetermined;
  return status;
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
void SystemGeolocationSourceOhos::SetSystemPermission(
    device::LocationSystemPermissionStatus status) {
  if (CachedStatus() == status) {
    return;
  }
  CachedStatus() = status;
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
    callback_.Run(CachedStatus());
  }
}

}  // namespace chrome::ohos
