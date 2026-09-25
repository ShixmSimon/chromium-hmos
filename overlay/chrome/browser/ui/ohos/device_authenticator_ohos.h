// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Asking the user to prove who they are before a saved password is shown,
// copied, edited, exported or filled into a page.
//
// The prompt itself is HarmonyOS's -- face, fingerprint or the lock-screen
// credential, whichever the user has enrolled -- reached through the shell's
// UserAuthService. Chromium never draws a password field of its own.
//
// Separate from the HUKS key that protects Login Data on disk: that one is
// opened once at startup without asking anything, or the browser would demand
// a fingerprint every time it launched.

#ifndef CHROME_BROWSER_UI_OHOS_DEVICE_AUTHENTICATOR_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_DEVICE_AUTHENTICATOR_OHOS_H_

#include <memory>

#include "components/device_reauth/device_authenticator.h"

namespace chrome::ohos {

// A fresh authenticator for one caller. Null when the shell is not attached,
// which leaves the caller to refuse rather than to proceed unprotected.
std::unique_ptr<device_reauth::DeviceAuthenticator> MakeOhosDeviceAuthenticator(
    const device_reauth::DeviceAuthParams& params);

// Whether this device can ask at all: false when the user has set no lock
// screen and enrolled no biometric, in which case stored passwords stay shut
// rather than opening to anyone holding the phone.
bool CanAuthenticateOnThisDevice();

// Asks the shell whether anything is enrolled, and remembers the answer for
// the synchronous callers above. Cheap; called at startup and whenever the
// shell says the device may have changed.
void RefreshAuthenticationAvailability();

// Forgets the last successful authentication, so the next action asks again.
// Called when the app goes to the background, when the screen locks, and when
// the shell moves between ordinary and incognito windows.
void ForgetRecentAuthentication(const char* reason);

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_DEVICE_AUTHENTICATOR_OHOS_H_
