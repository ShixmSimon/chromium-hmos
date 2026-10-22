// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef COMPONENTS_COMPONENT_UPDATER_OHOS_SECURITY_COMPONENT_POLICY_H_
#define COMPONENTS_COMPONENT_UPDATER_OHOS_SECURITY_COMPONENT_POLICY_H_

#include <string_view>

namespace component_updater {
// Data-only components with upstream-pinned CRX signing keys. Keep this gate
// at the service boundary as well as registration: other features can attempt
// to register their own components later in startup.
inline constexpr bool IsOhosSecurityComponent(std::string_view id) {
  return id == "hfnkpimlhhgieaddgfemjhofmfblmnib" ||  // CRLSet
         id == "efniojlnjndmcbiieegkicadnoecjjef";    // PKI Metadata
}
}  // namespace component_updater

#endif  // COMPONENTS_COMPONENT_UPDATER_OHOS_SECURITY_COMPONENT_POLICY_H_
