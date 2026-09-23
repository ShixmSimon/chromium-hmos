// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_FIDO_OHOS_AUTHENTICATOR_CONFIG_H_
#define DEVICE_FIDO_OHOS_AUTHENTICATOR_CONFIG_H_

#include "base/files/file_path.h"

namespace device::fido::ohos {

// Per-profile configuration of the HarmonyOS platform authenticator, the
// counterpart of fido::mac::AuthenticatorConfig.
struct AuthenticatorConfig {
  // The profile directory; its passkey metadata file lives there, so each
  // profile sees only its own passkeys.
  base::FilePath profile_path;
};

}  // namespace device::fido::ohos

#endif  // DEVICE_FIDO_OHOS_AUTHENTICATOR_CONFIG_H_
