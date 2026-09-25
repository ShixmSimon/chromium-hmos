// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/shell_password_cleanup_ohos.h"

#include "base/location.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "chrome/browser/password_manager/factories/profile_password_store_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ohos/huks_key_provider_ohos.h"
#include "components/password_manager/core/browser/password_store/password_store_interface.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"

namespace chrome::ohos {

namespace {

// Set once the sweep has run. That pref is the whole condition: what is being
// cleared is whatever the shared key wrote, and the first start of a build
// carrying this code is exactly the moment to do it.
//
// Not conditioned on whether HUKS is now providing the key -- it is, in the
// same build -- because that would mean the sweep never ran and the old
// credentials stayed, readable by the key every Chromium build shares.
constexpr char kSweptPref[] = "ohos.passwords_saved_without_protection_cleared";

}  // namespace

void RegisterPasswordCleanupPrefs(PrefRegistrySimple* registry) {
  registry->RegisterBooleanPref(kSweptPref, false);
  // On by default: a saved password should not go into a page until the
  // person holding the phone has shown they are the person who saved it.
  registry->RegisterBooleanPref("ohos.password_fill_requires_auth", true);
}

void ClearPasswordsSavedWithoutProtection(Profile* profile) {
  if (!profile || profile->IsOffTheRecord()) {
    return;
  }
  PrefService* prefs = profile->GetPrefs();
  if (!prefs) {
    return;
  }
  // Swept once -- unless the wrapping key was lost, in which case what is
  // stored cannot be decrypted by anything and has to go however many times
  // it happens.
  if (prefs->GetBoolean(kSweptPref) && !WrappedKeyWasReplaced()) {
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
      base::BindOnce(
          [](bool replaced, bool success) {
            LOG(WARNING) << "OHOS passwords: cleared stored credentials -- "
                         << (replaced ? "the key that protected them is gone "
                                        "(new device, restore or reinstall)"
                                      : "they predate any real protection")
                         << ", success=" << success;
          },
          WrappedKeyWasReplaced()));
  prefs->SetBoolean(kSweptPref, true);
}

}  // namespace chrome::ohos
