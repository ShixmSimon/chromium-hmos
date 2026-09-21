// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/permissions/system/system_permission_settings_ohos.h"

#include <map>
#include <memory>
#include <utility>

#include "base/no_destructor.h"
#include "chrome/browser/permissions/system/geolocation_observation.h"
#include "chrome/browser/permissions/system/platform_handle.h"
#include "chrome/browser/permissions/system/system_permission_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"

namespace system_permission_settings {

namespace {

// Browser UI thread only, on both sides and without a lock. Writes arrive
// through ExecuteAuraShellBrowserCommand, which posts every command to the UI
// task runner; readers are permission contexts and content-settings UI, which
// Chromium already confines there. Worth stating because nothing in the types
// enforces it -- a DCHECK would, but this build compiles those out, so a
// future caller from another thread would produce stale reads rather than a
// crash.
std::map<ContentSettingsType, SystemPermission>& PermissionStates() {
  static base::NoDestructor<std::map<ContentSettingsType, SystemPermission>>
      states;
  return *states;
}

OhosPermissionRequester& Requester() {
  static base::NoDestructor<OhosPermissionRequester> requester;
  return *requester;
}

base::RepeatingClosure& SettingsOpener() {
  static base::NoDestructor<base::RepeatingClosure> opener;
  return *opener;
}

// Only the permissions the shell knows how to ask for are gated here. For
// anything else the honest answer is the one the default implementation gives:
// claiming a capability is blocked when nothing has established that would
// suppress prompts the user ought to see.
bool IsManaged(ContentSettingsType type) {
  return PermissionStates().contains(type);
}

class PlatformHandleImpl : public PlatformHandle {
  bool CanPrompt(ContentSettingsType type) override {
    return IsSystemPermissionPrompt(GetOhosSystemPermission(type));
  }

  bool IsDenied(ContentSettingsType type) override {
    return IsManaged(type) &&
           IsSystemPermissionDenied(GetOhosSystemPermission(type));
  }

  bool IsAllowed(ContentSettingsType type) override {
    return !IsManaged(type) ||
           IsSystemPermissionAllowed(GetOhosSystemPermission(type));
  }

  void IsDeniedFresh(ContentSettingsType type,
                     SystemPermissionDeniedCallback callback) override {
    // The shell reports on its own whenever the state changes, so what is held
    // here is already as fresh as a round trip would make it.
    std::move(callback).Run(IsDenied(type));
  }

  void OpenSystemSettings(content::WebContents*,
                          ContentSettingsType type) override {
    // The default implementation reaches NOTREACHED() here, which this build
    // compiles to a trap: the content-settings bubble offers this as its
    // primary button, so a user who took it would lose the browser.
    if (SettingsOpener()) {
      SettingsOpener().Run();
    }
  }

  void Request(ContentSettingsType type,
               SystemPermissionResponseCallback callback) override {
    // Also NOTREACHED() by default, and reachable from the <permission>
    // element without going through any platform-gated UI.
    if (Requester()) {
      Requester().Run(type, std::move(callback));
      return;
    }
    // Nothing to ask with. The callback carries no result -- the caller
    // re-reads the state -- so running it now leaves that caller with the
    // truth rather than waiting on an answer that is not coming.
    std::move(callback).Run();
  }

  std::unique_ptr<ScopedObservation> Observe(
      SystemPermissionChangedCallback observer) override {
    // GeolocationObservation is compiled in because OHOS now sets
    // os_level_geolocation_permission_supported, and it already forwards
    // GeolocationSystemPermissionManager updates in this exact shape.
    return std::make_unique<GeolocationObservation>(std::move(observer));
  }
};

}  // namespace

SystemPermission GetOhosSystemPermission(ContentSettingsType type) {
  const auto it = PermissionStates().find(type);
  return it == PermissionStates().end() ? SystemPermission::kNotDetermined
                                        : it->second;
}

void SetOhosSystemPermission(ContentSettingsType type, SystemPermission state) {
  PermissionStates()[type] = state;
}

void SetOhosPermissionRequester(OhosPermissionRequester requester) {
  Requester() = std::move(requester);
}

void SetOhosSettingsOpener(base::RepeatingClosure opener) {
  SettingsOpener() = std::move(opener);
}

std::unique_ptr<PlatformHandle> PlatformHandle::Create() {
  return std::make_unique<PlatformHandleImpl>();
}

}  // namespace system_permission_settings
