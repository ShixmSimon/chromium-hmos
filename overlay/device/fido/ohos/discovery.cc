// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/fido/ohos/discovery.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "device/fido/ohos/authenticator.h"

namespace device::fido::ohos {

FidoOhosPlatformDiscovery::FidoOhosPlatformDiscovery(AuthenticatorConfig config)
    : FidoDiscoveryBase(FidoTransportProtocol::kInternal),
      config_(std::move(config)) {}

FidoOhosPlatformDiscovery::~FidoOhosPlatformDiscovery() = default;

void FidoOhosPlatformDiscovery::Start() {
  DCHECK(!authenticator_);
  if (!observer()) {
    return;
  }
  // Asked per request rather than cached: the user can set or remove the lock
  // screen while the browser runs.
  OhosPlatformAuthenticator::IsAvailable(
      base::BindOnce(&FidoOhosPlatformDiscovery::OnAvailabilityChecked,
                     weak_factory_.GetWeakPtr()));
}

void FidoOhosPlatformDiscovery::OnAvailabilityChecked(bool is_available) {
  if (!is_available) {
    observer()->DiscoveryStarted(this, /*success=*/false);
    return;
  }
  authenticator_ = std::make_unique<OhosPlatformAuthenticator>(config_);
  observer()->DiscoveryStarted(this, /*success=*/true, {authenticator_.get()});
}

}  // namespace device::fido::ohos
