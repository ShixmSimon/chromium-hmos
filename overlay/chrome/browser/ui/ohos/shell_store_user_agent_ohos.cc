// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/shell_store_user_agent_ohos.h"

#include <string>

#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "components/embedder_support/user_agent_utils.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "url/gurl.h"

namespace chrome::ohos {

namespace {

// The store answers on two hosts: the one it moved to, and the old path on
// chrome.google.com that still redirects there. Matching the old one too
// means the first navigation already carries the right UA rather than
// getting it on the redirect.
bool IsChromeWebStore(const GURL& url) {
  if (!url.SchemeIs("https")) {
    return false;
  }
  if (url.host() == "chromewebstore.google.com") {
    return true;
  }
  return url.host() == "chrome.google.com" &&
         url.path().starts_with("/webstore");
}

class StoreUserAgentWatcher
    : public content::WebContentsObserver,
      public content::WebContentsUserData<StoreUserAgentWatcher> {
 public:
  ~StoreUserAgentWatcher() override = default;

  void DidStartNavigation(content::NavigationHandle* handle) override {
    // Only the tab's own page. A same-document navigation keeps the UA it
    // committed with, and setting the flag on one does nothing.
    if (!handle->IsInPrimaryMainFrame() || handle->IsSameDocument()) {
      return;
    }
    // "Desktop site" uses this same mechanism, and when the reader has asked
    // for it the override in place is theirs, not ours. Tell the two apart
    // by the string rather than by remembering a flag, because they can turn
    // it on and off while the tab sits on the store.
    const std::string current =
        web_contents()->GetUserAgentOverride().ua_string_override;
    const bool ours = !store_ua_.empty() && current == store_ua_;

    if (!IsChromeWebStore(handle->GetURL())) {
      if (ours) {
        RestoreUserAgent(handle);
      }
      return;
    }

    if (!ours) {
      // A phone gets the store's desktop site only when the reader asked for
      // desktop sites. The store refuses to install anything to a mobile UA,
      // so on a tablet or a PC there is nothing to weigh -- but on a phone
      // the desktop store is a worse page to read, and this is the switch
      // the reader already has for saying they want it anyway.
      desktop_site_ =
          current == embedder_support::GetUserAgentForOhos(/*mobile=*/false);
      if (IsAuraShellMobilePhoneUi() && !desktop_site_) {
        return;
      }
    }

    store_ua_ = embedder_support::GetOhosStoreUserAgent();
    blink::UserAgentOverride store;
    store.ua_string_override = store_ua_;
    // The metadata is the ordinary desktop one. The UA still names
    // OpenHarmony, so platform HarmonyOS agrees with it -- and a UA-CH
    // platform that disagrees with the UA string is what bot checks read as
    // a spoofed browser, which this port has been bitten by once already.
    store.ua_metadata_override =
        embedder_support::GetUserAgentMetadataForOhos(/*mobile=*/false);
    web_contents()->SetUserAgentOverride(store, /*override_in_new_tabs=*/false);
    handle->SetIsOverridingUserAgent(true);
  }

 private:
  friend class content::WebContentsUserData<StoreUserAgentWatcher>;

  // Hands the tab back the user agent it had before the store took it.
  //
  // Clearing the flag alone is not enough any more: on a phone this watcher
  // only ever takes over from "desktop site", so leaving the store's string
  // installed and unused would read to everything else as the reader having
  // switched desktop sites off.
  void RestoreUserAgent(content::NavigationHandle* handle) {
    blink::UserAgentOverride restored;
    if (desktop_site_) {
      restored.ua_string_override =
          embedder_support::GetUserAgentForOhos(/*mobile=*/false);
      restored.ua_metadata_override =
          embedder_support::GetUserAgentMetadataForOhos(/*mobile=*/false);
    }
    web_contents()->SetUserAgentOverride(restored,
                                         /*override_in_new_tabs=*/false);
    handle->SetIsOverridingUserAgent(desktop_site_);
    store_ua_.clear();
  }

  explicit StoreUserAgentWatcher(content::WebContents* contents)
      : content::WebContentsObserver(contents),
        content::WebContentsUserData<StoreUserAgentWatcher>(*contents) {}

  // The last override this watcher installed, empty until it installs one.
  std::string store_ua_;
  // Whether the tab was asking for desktop sites when the store took over,
  // and so what to hand back when it leaves.
  bool desktop_site_ = false;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(StoreUserAgentWatcher);

}  // namespace

void WatchChromeWebStoreUserAgent(content::WebContents* contents) {
  if (!contents) {
    return;
  }
  // Phones are watched too now. Whether the store's desktop user agent is
  // actually sent is decided per navigation, by whether the reader has
  // asked this tab for desktop sites.
  StoreUserAgentWatcher::CreateForWebContents(contents);
}

}  // namespace chrome::ohos
