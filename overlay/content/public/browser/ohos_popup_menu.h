// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_BROWSER_OHOS_POPUP_MENU_H_
#define CONTENT_PUBLIC_BROWSER_OHOS_POPUP_MENU_H_

#include <stdint.h>

#include <vector>

#include "base/functional/callback.h"
#include "content/common/content_export.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "third_party/blink/public/mojom/choosers/popup_menu.mojom.h"

namespace content {

class RenderFrameHost;

// Shows a <select>'s options with the HarmonyOS shell's own picker.
//
// Upstream sends an external popup menu to RenderViewHostDelegateView, which
// only declares ShowPopupMenu with USE_EXTERNAL_POPUP_MENU (Android, Mac);
// turning that on here would rebuild much of content. On OHOS
// RenderFrameHostImpl::ShowPopupMenu calls this handler instead, which chrome
// sets at startup. The handler answers through `client`: DidAcceptIndices
// with indices into `items`, or DidCancel.
using OhosPopupMenuHandler = base::RepeatingCallback<void(
    RenderFrameHost* frame,
    mojo::PendingRemote<blink::mojom::PopupMenuClient> client,
    int32_t selected_item,
    std::vector<blink::mojom::MenuItemPtr> items,
    bool allow_multiple_selection)>;

// Defined in render_frame_host_impl.cc. A null handler cancels every menu.
CONTENT_EXPORT void SetOhosPopupMenuHandler(OhosPopupMenuHandler handler);

}  // namespace content

#endif  // CONTENT_PUBLIC_BROWSER_OHOS_POPUP_MENU_H_
