// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_PASSWORD_VAULT_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_PASSWORD_VAULT_OHOS_H_

#include "components/autofill/core/common/mojom/autofill_types.mojom-shared.h"
#include "components/autofill/core/common/unique_ids.h"

namespace password_manager {
class ContentPasswordManagerDriver;
class PasswordFormCache;
struct PasswordForm;
}  // namespace password_manager

namespace chrome::ohos {

// HarmonyOS's Password Vault, for pages drawn by Chromium rather than ArkUI.
//
// The vault only answers apps that describe their login fields to it through
// autoFillManager: reporting a field to the IME as a username or password
// changes the keyboard and nothing else. So the engine HAR's "passwordvault"
// service makes those calls, and this is the Chromium side of it. Passwords
// go from Chromium to the HAR and back; the shell never sees one.
//
// Only used while password_manager::IsOhosPasswordVaultOn() is true.

// A field in a password form was focused. Asks the vault for an account once
// per form, and fills the form with the one the user picks.
void MaybeOfferVaultFill(
    password_manager::ContentPasswordManagerDriver* driver,
    password_manager::PasswordFormCache* forms,
    autofill::FieldRendererId focused_field,
    autofill::mojom::FocusedFieldType focused_field_type);

// A login succeeded with `credentials`. The vault asks the user whether to
// save or update them.
void OfferVaultSave(const password_manager::PasswordForm& credentials);

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_PASSWORD_VAULT_OHOS_H_
