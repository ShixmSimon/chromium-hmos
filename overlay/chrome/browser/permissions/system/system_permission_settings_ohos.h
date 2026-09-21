// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PERMISSIONS_SYSTEM_SYSTEM_PERMISSION_SETTINGS_OHOS_H_
#define CHROME_BROWSER_PERMISSIONS_SYSTEM_SYSTEM_PERMISSION_SETTINGS_OHOS_H_

#include "base/functional/callback.h"
#include "chrome/browser/permissions/system/system_permission_common.h"

namespace system_permission_settings {

// The HarmonyOS side of the system permission layer.
//
// Chromium asks these questions from the browser process, but only ArkTS can
// answer them: abilityAccessCtrl needs a UIAbility context, and the NDK
// equivalent lives in libability_runtime, which //chrome/browser does not
// link. So the shell pushes what it knows through the runtime bridge, and the
// bridge installs the two closures that let a question travel the other way.
//
// This header is the meeting point rather than the bridge's own, because
// PlatformHandle is built below //chrome/browser:core and cannot include from
// it.

// The app's permission for `type`, as last reported by the shell. Defaults to
// kNotDetermined, which reads as "may prompt" rather than as a denial.
SystemPermission GetOhosSystemPermission(ContentSettingsType type);
void SetOhosSystemPermission(ContentSettingsType type, SystemPermission state);

// Asks the shell to run a permission request and to invoke the closure once
// the user has answered. Unset until the bridge installs it.
using OhosPermissionRequester =
    base::RepeatingCallback<void(ContentSettingsType type,
                                 base::OnceClosure done)>;
void SetOhosPermissionRequester(OhosPermissionRequester requester);

// Asks the shell to open the app's own settings page.
void SetOhosSettingsOpener(base::RepeatingClosure opener);

}  // namespace system_permission_settings

#endif  // CHROME_BROWSER_PERMISSIONS_SYSTEM_SYSTEM_PERMISSION_SETTINGS_OHOS_H_
