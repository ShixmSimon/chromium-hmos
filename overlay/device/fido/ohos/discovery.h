// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_FIDO_OHOS_DISCOVERY_H_
#define DEVICE_FIDO_OHOS_DISCOVERY_H_

#include <memory>

#include "base/component_export.h"
#include "base/memory/weak_ptr.h"
#include "device/fido/fido_discovery_base.h"
#include "device/fido/ohos/authenticator_config.h"

namespace device::fido::ohos {

class OhosPlatformAuthenticator;

// Offers the HarmonyOS platform authenticator to a WebAuthn request when the
// device can verify its user, as FidoTouchIdDiscovery does on macOS.
class COMPONENT_EXPORT(DEVICE_FIDO) FidoOhosPlatformDiscovery
    : public FidoDiscoveryBase {
 public:
  explicit FidoOhosPlatformDiscovery(AuthenticatorConfig config);
  FidoOhosPlatformDiscovery(const FidoOhosPlatformDiscovery&) = delete;
  FidoOhosPlatformDiscovery& operator=(const FidoOhosPlatformDiscovery&) =
      delete;
  ~FidoOhosPlatformDiscovery() override;

  // FidoDiscoveryBase:
  void Start() override;

 private:
  void OnAvailabilityChecked(bool is_available);

  const AuthenticatorConfig config_;
  std::unique_ptr<OhosPlatformAuthenticator> authenticator_;
  base::WeakPtrFactory<FidoOhosPlatformDiscovery> weak_factory_{this};
};

}  // namespace device::fido::ohos

#endif  // DEVICE_FIDO_OHOS_DISCOVERY_H_
