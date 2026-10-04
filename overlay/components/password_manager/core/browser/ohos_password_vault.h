// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_PASSWORD_MANAGER_CORE_BROWSER_OHOS_PASSWORD_VAULT_H_
#define COMPONENTS_PASSWORD_MANAGER_CORE_BROWSER_OHOS_PASSWORD_VAULT_H_

#include "components/prefs/pref_service.h"

namespace password_manager {

// Whether HarmonyOS's Password Vault fills and saves passwords instead of
// Chromium's own suggestions and save prompt. The shell's settings page owns
// it; off by default. While it is on, Chromium still keeps a silent copy of
// every password the vault is offered, so turning it off loses nothing.
//
// Registered by chrome/browser/ui/ohos/shell_password_cleanup_ohos.cc; read
// here, without a pref name constant in components/, because components/
// cannot see chrome/.
inline constexpr char kOhosPasswordVaultPref[] = "ohos.password_vault_enabled";

inline bool IsOhosPasswordVaultOn(const PrefService* prefs) {
  const PrefService::Preference* pref =
      prefs ? prefs->FindPreference(kOhosPasswordVaultPref) : nullptr;
  return pref && pref->GetValue()->GetIfBool().value_or(false);
}

}  // namespace password_manager

#endif  // COMPONENTS_PASSWORD_MANAGER_CORE_BROWSER_OHOS_PASSWORD_VAULT_H_
