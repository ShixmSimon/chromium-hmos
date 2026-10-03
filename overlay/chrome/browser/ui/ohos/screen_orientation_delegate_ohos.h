// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OHOS_SCREEN_ORIENTATION_DELEGATE_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SCREEN_ORIENTATION_DELEGATE_OHOS_H_

#include "content/public/browser/screen_orientation_delegate.h"

namespace content {
class WebContents;
}

// screen.orientation.lock() on HarmonyOS: the engine HAR's OrientationService
// sets the app window's preferred orientation, and restores the shell's own
// preference on unlock.
//
// As in Chrome on Android, a page must be fullscreen to lock; leaving
// fullscreen unlocks. Desktop (2in1) windows do not rotate and report the
// API as unsupported.
class ScreenOrientationDelegateOhos : public content::ScreenOrientationDelegate {
 public:
  ScreenOrientationDelegateOhos();
  ScreenOrientationDelegateOhos(const ScreenOrientationDelegateOhos&) = delete;
  ScreenOrientationDelegateOhos& operator=(
      const ScreenOrientationDelegateOhos&) = delete;
  ~ScreenOrientationDelegateOhos() override;

  // content::ScreenOrientationDelegate:
  bool FullScreenRequired(content::WebContents* web_contents) override;
  void Lock(content::WebContents* web_contents,
            device::mojom::ScreenOrientationLockType lock_orientation) override;
  bool ScreenOrientationProviderSupported(
      content::WebContents* web_contents) override;
  void Unlock(content::WebContents* web_contents) override;
};

namespace chrome::ohos {

// A landscape video that a phone page makes fullscreen turns the screen to
// landscape until it leaves fullscreen, as Chrome on Android does. Android
// does it in Blink from a WebPreferences field that exists only on Android;
// here the browser watches the tab. Attach once per tab (TabHelpers).
void WatchFullscreenVideoOrientation(content::WebContents* web_contents);

}  // namespace chrome::ohos

#endif  // CHROME_BROWSER_UI_OHOS_SCREEN_ORIENTATION_DELEGATE_OHOS_H_
