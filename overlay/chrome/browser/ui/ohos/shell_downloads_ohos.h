// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_DOWNLOADS_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_DOWNLOADS_OHOS_H_

#include <string>

class Profile;

namespace chrome::ohos {

// Downloads as the ArkUI shell sees them. The shell draws its own downloads
// page and progress UI from downloadUpdated / downloadList events; the
// command and observer halves are declared in shell_services_ohos.h. This
// header holds what upstream Chromium files call, so it stays light: patched
// files include it without pulling in the shell services.

// Whether the shell draws download UI, in which case Chromium's own (the
// toolbar button, its bubble and animations) stays hidden while downloads
// run as usual. Phones hand download UI to the shell; other devices keep
// Chromium's. --ohos-download-ui=shell|native overrides that either way.
bool ShouldShellDrawDownloadUi();

// Points the download and "save page as" directories at the one the shell
// passed in --ohos-download-dir, and turns off the "where to save" prompt,
// since the phone has no desktop file dialog to answer it. Takes a plain path
// or a HarmonyOS "file://docs/..." URI. Applies whatever directory is pending
// (the switch, or a later SetShellDownloadDirectory) that has not been applied
// yet, to the original profile; cheap to call often.
void ApplyShellDownloadDirectory(Profile* profile);

// The same at run time, for the setDownloadDirectory command: HarmonyOS only
// hands out the Download/<bundle> directory once the shell's UI is up, after
// Chromium has started -- possibly before any profile exists, in which case
// `profile` is null and the directory is applied by the next
// ApplyShellDownloadDirectory(). False when `location` is not usable.
bool SetShellDownloadDirectory(Profile* profile, const std::string& location);

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_DOWNLOADS_OHOS_H_
