// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_FIDO_OHOS_USER_AUTH_H_
#define DEVICE_FIDO_OHOS_USER_AUTH_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback_forward.h"

namespace device::fido::ohos {

// User verification through UserAuthenticationKit, which exists only in
// ArkTS. Both calls go over components/ohos_system_service to the engine's
// "userauth" handler (UserAuthService.ets), and both reply on the calling
// sequence.

// Whether the device user can authenticate at ATL3 with face, fingerprint or
// the lock-screen PIN. False when ArkTS is not attached.
void IsUserAuthAvailable(base::OnceCallback<void(bool)> callback);

// Shows the system authentication prompt, bound to `challenge` (the HUKS
// session challenge), with `title` as its heading. Replies with the AuthToken,
// or nullopt if the user cancelled or failed.
void AuthenticateUser(
    const std::vector<uint8_t>& challenge,
    const std::string& title,
    base::OnceCallback<void(std::optional<std::vector<uint8_t>>)> callback);

}  // namespace device::fido::ohos

#endif  // DEVICE_FIDO_OHOS_USER_AUTH_H_
