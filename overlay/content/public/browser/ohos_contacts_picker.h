// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_BROWSER_OHOS_CONTACTS_PICKER_H_
#define CONTENT_PUBLIC_BROWSER_OHOS_CONTACTS_PICKER_H_

#include <optional>
#include <vector>

#include "base/functional/callback.h"
#include "content/common/content_export.h"
#include "third_party/blink/public/mojom/contacts/contacts_manager.mojom.h"

namespace content {

class RenderFrameHost;

// navigator.contacts.select() on HarmonyOS: the shell shows the system
// contact picker. ContactsManagerImpl has a provider only on Android; on
// OHOS it calls this handler, which chrome sets at startup. `done` gets the
// chosen contacts, an empty list when the user cancelled, or nullopt when no
// picker could be shown.
using OhosContactsPickerHandler = base::RepeatingCallback<void(
    RenderFrameHost* frame,
    bool multiple,
    bool include_names,
    bool include_emails,
    bool include_tel,
    bool include_addresses,
    bool include_icons,
    base::OnceCallback<void(
        std::optional<std::vector<blink::mojom::ContactInfoPtr>>)> done)>;

// Defined in contacts_manager_impl.cc. A null handler answers every request
// with nullopt.
CONTENT_EXPORT void SetOhosContactsPickerHandler(
    OhosContactsPickerHandler handler);

}  // namespace content

#endif  // CONTENT_PUBLIC_BROWSER_OHOS_CONTACTS_PICKER_H_
