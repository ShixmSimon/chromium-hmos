// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/shell_store_user_agent_ohos.h"

#include <string>

#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "base/system/sys_info.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "third_party/abseil-cpp/absl/cleanup/cleanup.h"
#include "components/embedder_support/user_agent_utils.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "net/http/http_request_headers.h"
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

// AppGallery's AppTest service rejects Chromium's normal OHOS UA as
// "device not support". Adding this token alone makes the same invitation
// succeed. Confine it to AppGallery documents; their cross-origin invitation
// requests use the document's UA too. Never advertise HuaweiBrowser or change
// Chromium's version, platform section, or client hints.
bool IsHuaweiAppGallery(const GURL& url) {
  return url.SchemeIs("https") && url.host() == "appgallery.huawei.com";
}

std::string AppGalleryUserAgent(std::string user_agent) {
  if (user_agent.find("ArkWeb/") == std::string::npos) {
    user_agent += " ArkWeb/7.0.0.107";
  }
  return user_agent;
}

// The client hints that go with the store's user agent.
//
// The store reads the platform from Sec-CH-UA-Platform and
// navigator.userAgentData, not from the User-Agent string, and sends
// Android to /unsupported whatever the string says. A phone reports
// Android there -- its platform follows --use-mobile-user-agent for the
// whole process, not the per-tab mobile flag -- so "desktop site" still
// arrived at the store as Android. The string carries X11, so the hints
// say Linux, with the kernel's version as Chrome on Linux reports it.
blink::UserAgentMetadata StoreUserAgentMetadata() {
  blink::UserAgentMetadata metadata =
      embedder_support::GetUserAgentMetadataForOhos(/*mobile=*/false);
  metadata.platform = "Linux";
  int32_t major = 0;
  int32_t minor = 0;
  int32_t bugfix = 0;
  base::SysInfo::OperatingSystemVersionNumbers(&major, &minor, &bugfix);
  metadata.platform_version =
      base::StringPrintf("%d.%d.%d", major, minor, bugfix);
  metadata.mobile = false;
  return metadata;
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
    if (IsHuaweiAppGallery(handle->GetURL()) && !store_ua_.empty() &&
        web_contents()->GetUserAgentOverride().ua_string_override == store_ua_) {
      RestoreUserAgent(handle);
    }
    if (ApplyAppGalleryUserAgent(handle)) {
      return;
    }
    // "Desktop site" uses this same mechanism, and when the reader has asked
    // for it the override in place is theirs, not ours. Tell the two apart
    // by the string rather than by remembering a flag, because they can turn
    // it on and off while the tab sits on the store.
    const std::string current =
        web_contents()->GetUserAgentOverride().ua_string_override;
    const bool ours = !store_ua_.empty() && current == store_ua_;

    const bool is_store = IsChromeWebStore(handle->GetURL());
    const std::string desktop =
        AuraShellDesktopSiteUserAgent().ua_string_override;
    bool applied = false;
    // Logged on the way out, whatever this navigation turns out to be. There
    // was no way to see from outside which of these decided the outcome, so
    // a store page that arrived mobile could not be told from one this never
    // looked at.
    absl::Cleanup say = [&] {
      LOG(WARNING) << "OHOS store UA: url=" << UrlForLog(handle->GetURL())
                   << " is_store=" << is_store
                   << " phone=" << IsAuraShellMobilePhoneUi()
                   << " ours=" << ours
                   << " desktop_site=" << desktop_site_
                   << " applied=" << applied
                   << " current_override=[" << current << "]"
                   << " store_ua=[" << (applied ? store_ua_ : std::string())
                   << "]";
    };

    if (!is_store) {
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
      desktop_site_ = current == desktop;
      if (IsAuraShellMobilePhoneUi() && !desktop_site_) {
        return;
      }
    }

    store_ua_ = embedder_support::GetOhosStoreUserAgent();
    blink::UserAgentOverride store;
    store.ua_string_override = store_ua_;
    // Linux, to agree with the X11 token in the string; see
    // StoreUserAgentMetadata() for why the ordinary metadata will not do.
    // Only on the store, so the mismatch a bot check could read as a spoofed
    // browser is confined to a site that checks the platform itself.
    store.ua_metadata_override = StoreUserAgentMetadata();
    web_contents()->SetUserAgentOverride(store, /*override_in_new_tabs=*/false);
    // Or the shell's next state poll follows the device back to a mobile
    // user agent, and the store's own script sends the tab to /unsupported.
    SetAuraShellUserAgentPinned(web_contents(), true);
    handle->SetIsOverridingUserAgent(true);
    applied = true;
  }

  void DidRedirectNavigation(content::NavigationHandle* handle) override {
    DidStartNavigation(handle);
  }

 private:
  friend class content::WebContentsUserData<StoreUserAgentWatcher>;

  // Restore the previous mode before the Chrome Web Store policy runs, so
  // navigating directly between the two stores keeps desktop mode intact.
  bool ApplyAppGalleryUserAgent(content::NavigationHandle* handle) {
    const blink::UserAgentOverride current =
        web_contents()->GetUserAgentOverride();
    const bool ours = !appgallery_ua_.empty() &&
                      current.ua_string_override == appgallery_ua_;
    if (!IsHuaweiAppGallery(handle->GetURL())) {
      if (ours) {
        web_contents()->SetUserAgentOverride(
            before_appgallery_, /*override_in_new_tabs=*/before_appgallery_pinned_);
        SetAuraShellUserAgentPinned(web_contents(), before_appgallery_pinned_);
        handle->SetIsOverridingUserAgent(
            !before_appgallery_.ua_string_override.empty());
      }
      appgallery_ua_.clear();
      return false;
    }
    // A manual mobile/desktop change while on this site becomes the new
    // baseline. Leaving must not undo the reader's latest choice.
    if (!ours) {
      before_appgallery_ = current;
      before_appgallery_pinned_ = IsAuraShellUserAgentPinned(web_contents());
    }
    blink::UserAgentOverride compatible = before_appgallery_;
    if (compatible.ua_string_override.empty()) {
      const bool mobile = IsAuraShellMobilePhoneUi();
      compatible.ua_string_override = embedder_support::GetUserAgentForOhos(mobile);
      compatible.ua_metadata_override =
          embedder_support::GetUserAgentMetadataForOhos(mobile);
    }
    compatible.ua_string_override =
        AppGalleryUserAgent(compatible.ua_string_override);
    appgallery_ua_ = compatible.ua_string_override;
    web_contents()->SetUserAgentOverride(compatible, /*override_in_new_tabs=*/false);
    SetAuraShellUserAgentPinned(web_contents(), true);
    handle->SetIsOverridingUserAgent(true);
    return true;
  }

  blink::UserAgentOverride before_appgallery_;
  bool before_appgallery_pinned_ = false;
  std::string appgallery_ua_;

  // What the store was actually sent, once the navigation is over. The
  // line at the start says what this watcher decided; this one says whether
  // it held, because on a 2in1 the store still received the plain desktop
  // string after this watcher had set the X11 one.
  void DidFinishNavigation(content::NavigationHandle* handle) override {
    if (!handle->IsInPrimaryMainFrame() || handle->IsSameDocument() ||
        !IsChromeWebStore(handle->GetURL())) {
      return;
    }
    const net::HttpRequestHeaders& headers = handle->GetRequestHeaders();
    content::NavigationEntry* entry =
        web_contents()->GetController().GetLastCommittedEntry();
    LOG(WARNING) << "OHOS store UA sent: url=" << UrlForLog(handle->GetURL())
                 << " committed=" << handle->HasCommitted()
                 << " entry_overriding="
                 << (entry && entry->GetIsOverridingUserAgent())
                 << " header_ua=["
                 << headers.GetHeader(net::HttpRequestHeaders::kUserAgent)
                        .value_or("-")
                 << "] header_platform=["
                 << headers.GetHeader("Sec-CH-UA-Platform").value_or("-")
                 << "] override=["
                 << web_contents()->GetUserAgentOverride().ua_string_override
                 << "]";
  }

  // Hands the tab back the user agent it had before the store took it.
  //
  // Clearing the flag alone is not enough any more: on a phone this watcher
  // only ever takes over from "desktop site", so leaving the store's string
  // installed and unused would read to everything else as the reader having
  // switched desktop sites off.
  void RestoreUserAgent(content::NavigationHandle* handle) {
    blink::UserAgentOverride restored;
    if (desktop_site_) {
      restored = AuraShellDesktopSiteUserAgent();
    }
    web_contents()->SetUserAgentOverride(restored,
                                         /*override_in_new_tabs=*/desktop_site_);
    // Still deliberate if desktop sites are what the reader asked for; back
    // to following the device if they are not.
    SetAuraShellUserAgentPinned(web_contents(), desktop_site_);
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
