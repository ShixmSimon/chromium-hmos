#!/usr/bin/env python3
"""Compile the production site UA policies and exercise its lifecycle."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'overlay/chrome/browser/ui/ohos/shell_store_user_agent_ohos.cc').read_text()

def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

policy = function(source, 'bool IsHuaweiAppGallery(') + '\n' + function(source, 'std::string AppGalleryUserAgent(')
policy += '\n' + function(source, 'bool IsGoogleService(')
policy += '\n' + function(source, 'std::string GoogleUserAgent(')
policy += '\nenum class SiteUserAgentPolicy { kNone, kAppGallery, kGoogle };\n'
policy += function(source, 'SiteUserAgentPolicy UserAgentPolicyFor(')
method = function(source, 'bool ApplySiteUserAgent(')
start_method = function(source, 'void DidStartNavigation(').replace(' override', '')
restore_method = function(source, 'void RestoreUserAgent(')
redirect_method = (function(source, 'void DidRedirectNavigation(').replace(' override', '')
                   if 'void DidRedirectNavigation(' in source else '')
store_policy = function(source, 'bool IsChromeWebStore(')
classifier = function((ROOT / 'overlay/chrome/browser/ui/ohos/aura_shell_runtime_bridge.cc').read_text(),
                      'bool IsAuraShellRequestingDesktopSite(')
viewport_patch = (ROOT / 'patches/ohos-mobile-ua-viewport.patch').read_text()
for script in ['ci-incremental-build.sh', 'apply-adapter.sh', 'check-upstream-adapter.sh']:
    assert 'patches/ohos-mobile-ua-viewport.patch' in (ROOT / 'scripts' / script).read_text(), script
viewport_added = '\n'.join(line[1:] for line in viewport_patch.splitlines()
                           if line.startswith('+') and not line.startswith('+++'))
viewport_desktop = re.search(r'  const bool desktop_site =[^;]+;', viewport_added)[0]
base_patch = (ROOT / 'patches/chromium-154-harmonyos.patch').read_text()
base_added = '\n'.join(line[1:] for line in base_patch.splitlines()
                       if line.startswith('+') and not line.startswith('+++'))
viewport_meta = re.search(r'  const bool viewport_meta_enabled =[^;]+;', base_added)[0]
viewport = ('bool MobileViewportEnabled(content::WebContents* web_contents) {\n' +
            viewport_desktop + '\n' + viewport_meta + '\n  return viewport_meta_enabled;\n}')
# The browser-client hook must use the same classifier as the shell UI.
viewport = viewport.replace('chrome::ohos::IsAuraShellRequestingDesktopSite',
                            'IsAuraShellRequestingDesktopSite')
unit = r'''
#include <cassert>
#include <optional>
#include <sstream>
#include <string>
#define LOG(level) std::ostringstream()
namespace absl {
template <typename F> struct Cleanup {
  F callback;
  Cleanup(F value) : callback(value) {}
  ~Cleanup() { callback(); }
};
}
struct GURL {
  std::string value;
  bool SchemeIs(const char* scheme) const {
    return value.starts_with(std::string(scheme) + "://");
  }
  bool SchemeIsHTTPOrHTTPS() const { return SchemeIs("https") || SchemeIs("http"); }
  bool DomainIs(const char* domain) const {
    const auto hostname = host();
    return hostname == domain || hostname.ends_with(std::string(".") + domain);
  }
  std::string host() const {
    auto start = value.find("://") + 3;
    return value.substr(start, value.find('/', start) - start);
  }
  std::string path() const {
    auto start = value.find('/', value.find("://") + 3);
    return start == std::string::npos ? "/" : value.substr(start);
  }
};
std::string UrlForLog(const GURL&) { return "test-origin"; }
namespace blink {
struct UserAgentMetadata {
  bool mobile;
  bool operator==(const UserAgentMetadata&) const = default;
};
struct UserAgentOverride {
  std::string ua_string_override;
  std::optional<UserAgentMetadata> ua_metadata_override;
  bool operator==(const UserAgentOverride&) const = default;
};
}
namespace content {
struct NavigationEntry {
  bool overriding = true;
  bool GetIsOverridingUserAgent() const { return overriding; }
};
struct NavigationController {
  NavigationEntry entry;
  const NavigationEntry* GetVisibleEntry() const { return &entry; }
};
struct WebContents {
  NavigationController controller;
  NavigationController& GetController() { return controller; }
  blink::UserAgentOverride ua;
  bool pinned = false;
  bool inherits = false;
  blink::UserAgentOverride GetUserAgentOverride() { return ua; }
  void SetUserAgentOverride(blink::UserAgentOverride value, bool inherit) {
    ua = value;
    inherits = inherit;
  }
};
struct NavigationHandle {
  GURL url;
  bool overriding = false;
  bool starting = true;
  bool main_frame = true;
  bool same_document = false;
  GURL GetURL() { return url; }
  bool IsInPrimaryMainFrame() const { return main_frame; }
  bool IsSameDocument() const { return same_document; }
  void SetIsOverridingUserAgent(bool value) {
    // The public NavigationHandle API permits this only in DidStartNavigation.
    assert(starting);
    overriding = value;
  }
};
struct WebContentsObserver {
  void DidRedirectNavigation(NavigationHandle*) {}
};
}
namespace embedder_support {
std::string GetUserAgentForOhos(bool mobile) {
  return mobile ? "Chrome/154.0.0.0 Mobile Safari/537.36 OpenHarmony/7.0"
                : "Chrome/154.0.0.0 Safari/537.36 OpenHarmony/7.0";
}
blink::UserAgentMetadata GetUserAgentMetadataForOhos(bool mobile) { return {mobile}; }
std::string GetOhosStoreUserAgent() { return "store desktop OpenHarmony/7.0"; }
}
blink::UserAgentMetadata StoreUserAgentMetadata() { return {false}; }
bool phone_ui = true;
bool IsAuraShellMobilePhoneUi() { return phone_ui; }
bool IsAuraShellUserAgentPinned(content::WebContents* tab) { return tab->pinned; }
void SetAuraShellUserAgentPinned(content::WebContents* tab, bool value) { tab->pinned = value; }
blink::UserAgentOverride AuraShellDesktopSiteUserAgent() {
  return {"desktop", blink::UserAgentMetadata{false}};
}
// The upstream Google classifier is a dependency, not reimplemented by the
// production policy. This fixture supplies representative upstream domains.
namespace google_util {
bool IsGoogleAssociatedDomainUrl(const GURL& url) {
  return url.DomainIs("google.com") || url.DomainIs("google.co.uk") ||
         url.DomainIs("google.com.hk") || url.DomainIs("youtube.com") ||
         url.DomainIs("googlevideo.com") || url.DomainIs("ytimg.com");
}
}
CLASSIFIER
VIEWPORT
POLICY
STORE_POLICY
struct Watcher : content::WebContentsObserver {
  content::WebContents* contents;
  content::WebContents* web_contents() { return contents; }
  blink::UserAgentOverride before_site_;
  bool before_site_pinned_ = false;
  std::string site_ua_;
  SiteUserAgentPolicy site_policy_ = SiteUserAgentPolicy::kNone;
  std::string store_ua_;
  blink::UserAgentOverride before_store_;
  bool before_store_pinned_ = false;
  bool desktop_site_ = false;
  explicit Watcher(content::WebContents* tab) : contents(tab) {}
METHOD
START_METHOD
RESTORE_METHOD
REDIRECT_METHOD
};
int main() {
  assert(IsHuaweiAppGallery({"https://appgallery.huawei.com/apptest/invite"}));
  for (const char* url : {"http://appgallery.huawei.com/apptest/", "https://example.com/",
                         "https://appgallery.huawei.com.evil.test/", "https://evilappgallery.huawei.com/"})
    assert(!IsHuaweiAppGallery({url}));
  const std::string token = " ArkWeb/7.0.0.107";
  assert(AppGalleryUserAgent("Chrome/154") == "Chrome/154" + token);
  assert(AppGalleryUserAgent("Chrome/154 ArkWeb/7.0.0.107") == "Chrome/154" + token);
  for (bool desktop : {false, true}) {
    for (bool empty : {false, true}) {
      content::WebContents tab;
      if (!empty) tab.ua = {desktop ? "desktop" : "mobile", blink::UserAgentMetadata{!desktop}};
      tab.pinned = desktop;
      auto baseline = tab.ua;
      Watcher watcher(&tab);
      content::NavigationHandle h{{"https://appgallery.huawei.com/apptest/invite"}, false};
      assert(watcher.ApplySiteUserAgent(&h));
      assert(tab.pinned && h.overriding && !tab.inherits);
      assert(tab.ua.ua_string_override.ends_with(token));
      assert(IsAuraShellRequestingDesktopSite(&tab) == (!empty && desktop));
      assert(MobileViewportEnabled(&tab) == (empty || !desktop));
      if (!empty) assert(tab.ua.ua_metadata_override == baseline.ua_metadata_override);
      auto compatible = tab.ua;
      assert(watcher.ApplySiteUserAgent(&h)); // refresh must not save the temporary UA
      assert(tab.ua == compatible);
      h.url.value = "https://example.com/";
      assert(!watcher.ApplySiteUserAgent(&h));
      assert(tab.ua == baseline && tab.pinned == desktop);
      assert(h.overriding == !empty);
    }
  }
  content::WebContents tab;
  Watcher watcher(&tab);
  content::NavigationHandle h{{"https://appgallery.huawei.com/"}, false};
  watcher.ApplySiteUserAgent(&h);
  tab.ua = {"new desktop", blink::UserAgentMetadata{false}}; // user changes mode
  tab.pinned = true;
  watcher.ApplySiteUserAgent(&h);
  h.url.value = "https://example.com/";
  watcher.ApplySiteUserAgent(&h);
  assert(tab.ua.ua_string_override == "new desktop" && tab.pinned);
  watcher.ApplySiteUserAgent(&h);
  assert(tab.ua.ua_string_override == "new desktop");

  for (const char* url : {"https://aistudio.google.com/", "https://accounts.google.com/",
                         "https://gemini.google.com/", "https://www.google.co.uk/",
                         "https://www.google.com.hk/", "https://m.youtube.com/",
                         "https://youtu.be/", "https://gmail.com/",
                         "https://ai.google.dev/", "https://www.google.org/",
                         "https://g.co/", "https://goo.gl/", "https://forms.gle/",
                         "https://about.google/", "https://example.goog/",
                         "https://www.withgoogle.com/", "https://www.youtube-nocookie.com/",
                         "https://mail.googlemail.com/", "https://googleapis.com/",
                         "https://content.googleapis.com/", "https://gstatic.com/",
                         "https://googleusercontent.com/"})
    assert(IsGoogleService({url}));
  for (const char* url : {"https://google.com.evil.test/", "https://notgoogle.com/",
                         "https://gmail.com.evil.test/", "https://evil-youtube.com/",
                         "https://example.com/google.com", "file://google.com/",
                         "https://google/", "https://notgoogle/", "https://about.google.evil.test/"})
    assert(!IsGoogleService({url}));
  assert(GoogleUserAgent("Chrome/155 Mobile Safari/537.36 OpenHarmony/7.0") ==
         "Chrome/155 Mobile Safari/537.36");
  assert(GoogleUserAgent("Chrome/155 OpenHarmony/7.0 Safari/537.36") ==
         "Chrome/155 Safari/537.36");
  assert(GoogleUserAgent("Chrome/155 OpenHarmony/7.0 OpenHarmony/8.0") == "Chrome/155");
  assert(GoogleUserAgent("Chrome/155 CustomOpenHarmony/7.0") ==
         "Chrome/155 CustomOpenHarmony/7.0");

  // Refresh, navigation between policies and manual mode changes must not
  // replace the original mode with a temporary compatibility UA.
  for (bool phone : {false, true}) {
    phone_ui = phone;
    for (bool desktop : {false, true}) {
      content::WebContents page;
      page.ua = {desktop ? "Chrome/155 Safari/537.36 OpenHarmony/7.0"
                         : "Chrome/155 Mobile Safari/537.36 OpenHarmony/7.0",
                 blink::UserAgentMetadata{!desktop}};
      page.pinned = desktop;
      const auto baseline = page.ua;
      Watcher policy(&page);
      content::NavigationHandle navigation{{"https://aistudio.google.com/"}};
      for (const char* destination : {"https://aistudio.google.com/",
                                      "https://aistudio.google.com/",
                                      "https://appgallery.huawei.com/",
                                      "https://m.youtube.com/",
                                      "https://chromewebstore.google.com/",
                                      "https://accounts.google.com/",
                                      "https://example.com/"}) {
        navigation.url.value = destination;
        policy.DidStartNavigation(&navigation);
        if (IsGoogleService(navigation.url)) {
          assert(page.ua.ua_string_override.find(" OpenHarmony/") == std::string::npos);
          assert(page.ua.ua_string_override.find(" ArkWeb/") == std::string::npos);
          if (!IsChromeWebStore(navigation.url))
            assert(page.ua.ua_metadata_override == baseline.ua_metadata_override);
        } else if (IsHuaweiAppGallery(navigation.url)) {
          assert(page.ua.ua_string_override == AppGalleryUserAgent(baseline.ua_string_override));
        } else {
          assert(page.ua == baseline && page.pinned == desktop);
        }
      }
      navigation.url.value = "https://gemini.google.com/";
      policy.DidStartNavigation(&navigation);
      page.ua = {"custom latest mode OpenHarmony/8.0", blink::UserAgentMetadata{desktop}};
      page.pinned = !desktop;
      auto latest = page.ua;
      policy.DidStartNavigation(&navigation);
      navigation.url.value = "https://example.com/";
      policy.DidStartNavigation(&navigation);
      assert(page.ua == latest && page.pinned == !desktop);
    }
  }
  // A pending entry may not have inherited the UA flag at navigation start.
  // The user's existing desktop override must still select the desktop store.
  {
    content::WebContents page;
    page.ua = AuraShellDesktopSiteUserAgent();
    page.pinned = true;
    page.controller.entry.overriding = false;
    Watcher policy(&page);
    content::NavigationHandle navigation{{"https://chromewebstore.google.com/"}};
    policy.DidStartNavigation(&navigation);
    assert(page.ua.ua_string_override == "store desktop");
  }

  // Device-following tabs have no explicit baseline override to restore.
  for (bool phone : {false, true}) {
    phone_ui = phone;
    content::WebContents page;
    Watcher policy(&page);
    content::NavigationHandle navigation{{"https://gemini.google.com/"}};
    policy.DidStartNavigation(&navigation);
    assert(page.ua.ua_string_override.find(" OpenHarmony/") == std::string::npos);
    assert(page.ua.ua_metadata_override->mobile == phone);
    for (const char* destination : {"https://chromewebstore.google.com/",
                                    "https://appgallery.huawei.com/",
                                    "https://aistudio.google.com/",
                                    "https://example.com/"}) {
      navigation.url.value = destination;
      policy.DidStartNavigation(&navigation);
    }
    assert(page.ua.ua_string_override.empty() && !page.pinned && !navigation.overriding);
    navigation.url.value = "https://aistudio.google.com/";
    navigation.main_frame = false;
    policy.DidStartNavigation(&navigation);
    assert(page.ua.ua_string_override.empty());
    navigation.main_frame = true;
    navigation.same_document = true;
    policy.DidStartNavigation(&navigation);
    assert(page.ua.ua_string_override.empty());
  }
  phone_ui = true;

  // Follow redirects away from either store, then start a fresh navigation.
  // Redirect callbacks must preserve the navigation's initial UA and must
  // never invoke the start-only NavigationHandle setter (which CHECKs in
  // Chromium). A fresh navigation must still restore the user's baseline.
  for (const char* origin : {"https://chromewebstore.google.com/",
                             "https://chrome.google.com/webstore/",
                             "https://appgallery.huawei.com/apptest/invite",
                             "https://aistudio.google.com/"}) {
    content::WebContents page;
    page.ua = AuraShellDesktopSiteUserAgent();
    page.pinned = true;
    auto baseline = page.ua;
    Watcher policy(&page);
    content::NavigationHandle navigation{{origin}};
    policy.DidStartNavigation(&navigation);
    assert(navigation.overriding && page.pinned);
    auto initial = page.ua;
    navigation.starting = false;
    for (const char* destination : {"https://example.com/redirect",
                                    "https://appgallery.huawei.com/",
                                    "https://chromewebstore.google.com/"}) {
      navigation.url.value = destination;
      policy.DidRedirectNavigation(&navigation);
      assert(page.ua == initial);
    }
    navigation.url.value = "https://example.com/";
    navigation.starting = true;
    policy.DidStartNavigation(&navigation);
    assert(page.ua == baseline && page.pinned && navigation.overriding);
  }
}
'''.replace('STORE_POLICY', store_policy).replace('POLICY', policy).replace('START_METHOD', start_method).replace('RESTORE_METHOD', restore_method).replace('REDIRECT_METHOD', redirect_method).replace('METHOD', method).replace('CLASSIFIER', classifier).replace('VIEWPORT', viewport)
with tempfile.TemporaryDirectory(prefix='appgallery-ua-') as directory:
    cc = Path(directory) / 'policy.cc'
    exe = Path(directory) / 'policy'
    cc.write_text(unit)
    subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++20', '-Wall', '-Wextra',
                    '-Werror', str(cc), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Google/AppGallery/Web Store UA hosts, token handling, metadata, cross-policy transitions, mode changes and restoration passed.')
