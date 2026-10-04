// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/password_vault_ohos.h"

#include <set>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "components/autofill/core/common/aliases.h"
#include "components/ohos_system_service/system_service_ohos.h"
#include "components/password_manager/content/browser/content_password_manager_driver.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/browser/password_form_cache.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/weak_document_ptr.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace chrome::ohos {

namespace {

// The HAR service; see PasswordVaultService.ets.
constexpr char kService[] = "passwordvault";

using autofill::FieldRendererId;
using autofill::mojom::FocusedFieldType;

// The fields one fill will write, in the document that asked.
struct FillTarget {
  content::WeakDocumentPtr document;
  url::Origin origin;
  FieldRendererId username;
  FieldRendererId password;
  // Sign-up forms: the "repeat the password" field, if any.
  FieldRendererId confirmation;
};

// The vault takes one request at a time -- a second one while its sheet is up
// fails with an internal error -- and a form it has already been asked about
// is not asked about again, or every move from the username to the password
// field would bring the sheet back.
struct VaultState {
  bool asking = false;
  std::set<
      std::pair<content::GlobalRenderFrameHostId, autofill::FormRendererId>>
      asked;
};

VaultState& State() {
  static base::NoDestructor<VaultState> state;
  return *state;
}

// Forms only matter while someone could still focus them; a long session
// would otherwise grow the set without end.
constexpr size_t kMaxAskedForms = 64;

void OnFillReply(FillTarget target, ohos_system_service::Reply reply) {
  State().asking = false;
  if (!reply.ok) {
    return;
  }
  // Nothing picked, or nothing saved for this site: the page stays as it is
  // and the user types.
  const base::DictValue& result = reply.result_dict();
  const std::string* password = result.FindString("password");
  if (!password || password->empty()) {
    return;
  }
  // The user may have gone elsewhere while the sheet was up. Fill only the
  // document that asked, and only while it is still on the same origin --
  // never whatever replaced it.
  content::RenderFrameHost* frame = target.document.AsRenderFrameHostIfValid();
  if (!frame || frame->GetLastCommittedOrigin() != target.origin) {
    return;
  }
  password_manager::ContentPasswordManagerDriver* driver =
      password_manager::ContentPasswordManagerDriver::GetForRenderFrameHost(
          frame);
  if (!driver) {
    return;
  }
  const std::string* username = result.FindString("username");
  const std::u16string user16 =
      username ? base::UTF8ToUTF16(*username) : std::u16string();
  const std::u16string password16 = base::UTF8ToUTF16(*password);
  // An empty username would clear whatever the user already typed there.
  driver->FillSuggestionById(
      user16.empty() ? FieldRendererId() : target.username, target.password,
      user16, password16,
      autofill::AutofillSuggestionTriggerSource::kFormControlElementClicked);
  if (target.confirmation) {
    driver->FillSuggestionById(
        FieldRendererId(), target.confirmation, std::u16string(), password16,
        autofill::AutofillSuggestionTriggerSource::kFormControlElementClicked);
  }
}

}  // namespace

void MaybeOfferVaultFill(password_manager::ContentPasswordManagerDriver* driver,
                         password_manager::PasswordFormCache* forms,
                         FieldRendererId focused_field,
                         FocusedFieldType focused_field_type) {
  // Usernames usually arrive as plain text fields: the renderer only calls a
  // field a username when Chromium has something saved for it, and in vault
  // mode Chromium hands the renderer nothing.
  if (focused_field_type != FocusedFieldType::kFillablePasswordField &&
      focused_field_type != FocusedFieldType::kFillableUsernameField &&
      focused_field_type != FocusedFieldType::kFillableNonSearchField) {
    return;
  }
  if (!driver || !forms || !focused_field || State().asking ||
      !ohos_system_service::IsAvailable()) {
    return;
  }
  const password_manager::PasswordForm* form =
      forms->GetPasswordForm(driver, focused_field);
  if (!form) {
    return;
  }

  // A login form takes a username and the current password. A sign-up form
  // takes a new password -- the vault offers a strong one -- and the username
  // if the user picks an account.
  const bool login = form->password_element_renderer_id &&
                     (focused_field == form->username_element_renderer_id ||
                      focused_field == form->password_element_renderer_id);
  const bool signup =
      !login && form->new_password_element_renderer_id &&
      (focused_field == form->username_element_renderer_id ||
       focused_field == form->new_password_element_renderer_id ||
       focused_field == form->confirmation_password_element_renderer_id);
  if (!login && !signup) {
    return;
  }

  content::RenderFrameHost* frame = driver->render_frame_host();
  const auto key =
      std::make_pair(frame->GetGlobalId(), form->form_data.renderer_id());
  VaultState& state = State();
  if (state.asked.contains(key)) {
    return;
  }
  if (state.asked.size() >= kMaxAskedForms) {
    state.asked.clear();
  }
  state.asked.insert(key);

  FillTarget target;
  target.document = frame->GetWeakDocumentPtr();
  target.origin = frame->GetLastCommittedOrigin();
  target.username = form->username_element_renderer_id;
  target.password = login ? form->password_element_renderer_id
                          : form->new_password_element_renderer_id;
  if (signup) {
    target.confirmation = form->confirmation_password_element_renderer_id;
  }

  base::DictValue args;
  // The vault matches accounts by this page's site.
  args.Set("url", form->url.spec());
  args.Set("kind", login ? "login" : "signup");
  args.Set("focus", focused_field == form->username_element_renderer_id
                        ? "username"
                        : "password");
  state.asking = true;
  ohos_system_service::Call(kService, "fill", std::move(args),
                            base::BindOnce(&OnFillReply, std::move(target)));
}

void OfferVaultSave(const password_manager::PasswordForm& credentials) {
  // Federated sign-ins have no password, and the vault matches by web site,
  // so only http(s) logins go.
  if (credentials.password_value.empty() ||
      !credentials.url.SchemeIsHTTPOrHTTPS() ||
      !ohos_system_service::IsAvailable()) {
    return;
  }
  base::DictValue args;
  args.Set("url", credentials.url.spec());
  args.Set("username", base::UTF16ToUTF8(credentials.username_value));
  args.Set("password", base::UTF16ToUTF8(credentials.password_value));
  ohos_system_service::Call(kService, "save", std::move(args),
                            base::DoNothing());
}

}  // namespace chrome::ohos
