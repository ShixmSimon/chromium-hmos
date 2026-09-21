// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SYSTEM_GEOLOCATION_SOURCE_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SYSTEM_GEOLOCATION_SOURCE_OHOS_H_

#include <memory>

#include "services/device/public/cpp/geolocation/location_system_permission_status.h"
#include "services/device/public/cpp/geolocation/system_geolocation_source.h"

namespace device {
class GeolocationSystemPermissionManager;
}

namespace chrome::ohos {

// Reports the HarmonyOS location permission to Chromium's platform-agnostic
// system permission layer.
//
// Without it, a site granted geolocation while the app itself holds no
// permission gets a prompt it cannot act on: the page is told yes and the
// platform then refuses. With it, GeolocationPermissionContextSystem resolves
// such a request to BLOCK before any prompt is shown.
//
// The state itself comes from the shell. The NDK can read it, but only through
// libability_runtime, which //chrome/browser does not link; ArkTS already has
// abilityAccessCtrl and a command channel back into the browser.
class SystemGeolocationSourceOhos : public device::SystemGeolocationSource {
 public:
  SystemGeolocationSourceOhos();

  SystemGeolocationSourceOhos(const SystemGeolocationSourceOhos&) = delete;
  SystemGeolocationSourceOhos& operator=(const SystemGeolocationSourceOhos&) =
      delete;

  ~SystemGeolocationSourceOhos() override;

  static std::unique_ptr<device::GeolocationSystemPermissionManager>
  CreateGeolocationSystemPermissionManager();

  // Called on the UI thread after the shell's report has been stored in
  // system_permission_settings. Safe to call before the source exists: the
  // state lives there, and the next source reads it on registration.
  static void NotifyPermissionChanged();

  // device::SystemGeolocationSource:
  void RegisterPermissionUpdateCallback(
      PermissionUpdateCallback callback) override;

 private:
  void NotifyCurrentStatus();

  PermissionUpdateCallback callback_;
};

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_SYSTEM_GEOLOCATION_SOURCE_OHOS_H_
