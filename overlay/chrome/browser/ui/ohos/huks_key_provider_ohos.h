// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The OSCrypt key for HarmonyOS, kept by HUKS.
//
// Until this existed the only provider here was PosixKeyProvider, whose key is
// a constant compiled into every Chromium build, so Login Data was obfuscated
// rather than encrypted and anyone with the file could read it offline.
//
// HUKS keys cannot be exported, and OSCrypt's KeyProvider must hand its key
// over for in-process use, so the two do not meet directly. The key Chromium
// uses is generated here and stored wrapped: HUKS holds a key that never
// leaves the secure world, and that key encrypts ours. What is on disk is
// useless without the device, which is the same bargain Keychain and DPAPI
// strike on macOS and Windows.
//
// What this does not do is keep the key out of process memory. Encrypting
// every value through HUKS would, and would mean changing OSCrypt's interface
// rather than implementing it.

#ifndef CHROME_BROWSER_UI_OHOS_HUKS_KEY_PROVIDER_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_HUKS_KEY_PROVIDER_OHOS_H_

#include <memory>

#include "components/os_crypt/async/browser/key_provider.h"

namespace chrome::ohos {

// Null when HUKS is unavailable, which leaves OSCrypt with the providers it
// had. Never returns a provider that would claim data is protected when it is
// not.
std::unique_ptr<os_crypt_async::KeyProvider> MakeHuksKeyProvider();

// Whether the key this build encrypts with is held by HUKS. False while only
// PosixKeyProvider is registered, which is what the password cleanup and the
// password manager switch both read.
bool PasswordsAreProtectedByHuks();

// Whether the wrapped key could not be opened and a new one was made, which
// happens on a new device, a restored backup, or a reinstall: HUKS keys do not
// travel, so the old wrapping key is gone and everything it protected is
// unreadable. Anything encrypted with the lost key has to go rather than sit
// there failing to decrypt forever.
bool WrappedKeyWasReplaced();

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_HUKS_KEY_PROVIDER_OHOS_H_
