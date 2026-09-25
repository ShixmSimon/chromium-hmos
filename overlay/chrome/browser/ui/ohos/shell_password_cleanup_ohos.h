// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Removes passwords that were saved before this platform had anywhere safe to
// keep them.
//
// OSCrypt on OHOS uses PosixKeyProvider, whose key is a constant compiled into
// every Chromium build, so Login Data is obfuscated rather than encrypted and
// anyone holding the file can read it offline. The password manager is off
// (ohos-password-manager-off.patch) so nothing new is written, but whatever a
// user saved while it was on is still there and still readable.
//
// This clears it once, at startup, and records that it has. When OSCrypt has a
// HUKS-backed key provider the sweep stops on its own: there will be nothing
// left that the old key wrote, and new passwords will be properly encrypted.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_PASSWORD_CLEANUP_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_PASSWORD_CLEANUP_OHOS_H_

class Profile;
class PrefRegistrySimple;

namespace chrome::ohos {

// Clears saved passwords unless they are already protected, and only the
// first time it is called for a profile. Safe to call on every startup.
void ClearPasswordsSavedWithoutProtection(Profile* profile);

void RegisterPasswordCleanupPrefs(PrefRegistrySimple* registry);

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_PASSWORD_CLEANUP_OHOS_H_
