// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SHELL_STORE_USER_AGENT_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_STORE_USER_AGENT_OHOS_H_

namespace content {
class WebContents;
}

namespace chrome::ohos {

// Tells the Chrome Web Store, and only it, that this is a desktop browser.
//
// The store picks the desktop or the mobile site from the User-Agent string
// alone. Client Hints make no difference to it: four UA shapes were compared
// on a tablet-sized window, and the only one that produced an install button
// was the one carrying a desktop windowing token --
//
//   PC; OpenHarmony 7.0; Android 10   mobile site, "add to home screen"
//   PC; OpenHarmony 7.0               mobile site
//   ... with UA-CH platform Linux     mobile site
//   PC; OpenHarmony 7.0; X11          desktop site, install button
//
// -- so without it an extension cannot be installed at all. One token is the
// whole difference.
//
// It cannot go in the global UA: with "X11; Linux" there, qq.com serves its
// phone pages. So it is added for this one site, on the navigation, and
// nowhere else.
//
// Phones are left alone: they have no extensions UI, and the store's desktop
// site would be unusable on one anyway.
//
// Idempotent, so it can be called whenever a tab becomes the current one.
void WatchChromeWebStoreUserAgent(content::WebContents* contents);

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_SHELL_STORE_USER_AGENT_OHOS_H_
