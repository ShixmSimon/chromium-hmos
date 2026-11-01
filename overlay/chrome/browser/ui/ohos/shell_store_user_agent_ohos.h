// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_STORE_USER_AGENT_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_STORE_USER_AGENT_OHOS_H_

namespace content {
class WebContents;
}

namespace chrome::ohos {

// Installs per-tab site UA compatibility policies:
// - Chrome Web Store: Linux desktop UA/Client Hints when desktop mode is used.
// - AppGallery: add the ArkWeb product required by its invitation service.
// - Google services (including YouTube): omit the OpenHarmony product token.
//
// Mobile/desktop mode and Client Hints are preserved by the general policies.
// Overrides are restored on the next main-frame navigation outside their
// scope. Redirects retain the initial navigation's UA, as required by the
// NavigationHandle API. Idempotent across repeated tab activation.
void WatchChromeWebStoreUserAgent(content::WebContents* contents);

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_STORE_USER_AGENT_OHOS_H_
