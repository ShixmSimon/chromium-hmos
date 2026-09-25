// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/shell_password_cleanup_ohos.h"

#include "base/location.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "chrome/browser/password_manager/factories/profile_password_store_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/password_manager/core/browser/password_store/password_store_interface.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"

namespace chrome::ohos {

namespace {

// Set once the sweep has run, so a user who saves nothing afterwards is not
// asked about it again and a later restart does not repeat the deletion.
constexpr char kSweptPref[] = "ohos.passwords_saved_without_protection_cleared";

// Whether Login Data's passwords are protected by anything worth the name.
// False while OSCrypt has only PosixKeyProvider, whose key is the same
// constant in every Chromium build ever shipped.
//
// This is the switch to flip when the HUKS key provider lands: after that the
// sweep stops happening and migration takes over.
bool PasswordsAreEncrypted() {
  return false;
}

}  // namespace

void RegisterPasswordCleanupPrefs(PrefRegistrySimple* registry) {
  registry->RegisterBooleanPref(kSweptPref, false);
}

void ClearPasswordsSavedWithoutProtection(Profile* profile) {
  if (!profile || profile->IsOffTheRecord() || PasswordsAreEncrypted()) {
    return;
  }
  PrefService* prefs = profile->GetPrefs();
  if (!prefs || prefs->GetBoolean(kSweptPref)) {
    return;
  }

  scoped_refptr<password_manager::PasswordStoreInterface> store =
      ProfilePasswordStoreFactory::GetForProfile(
          profile, ServiceAccessType::EXPLICIT_ACCESS);
  if (!store) {
    // No store, nothing saved, nothing to clear -- and nothing to remember
    // either: try again next time in case it simply was not up yet.
    return;
  }

  // Everything, not a window: the whole file was written with the shared key.
  store->RemoveLoginsCreatedBetween(
      FROM_HERE, base::Time(), base::Time::Max(),
      base::BindOnce([](bool success) {
        LOG(WARNING) << "OHOS passwords: cleared credentials saved before "
                        "this platform could protect them, success="
                     << success;
      }));
  prefs->SetBoolean(kSweptPref, true);
}

}  // namespace chrome::ohos
