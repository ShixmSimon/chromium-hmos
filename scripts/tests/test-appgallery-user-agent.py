#!/usr/bin/env python3
"""Compile the production AppGallery UA policy and exercise its lifecycle."""
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
method = function(source, 'bool ApplyAppGalleryUserAgent(')
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
template <typename F> struct Cleanup { F callback; ~Cleanup() { callback(); } };
}
struct GURL {
  std::string value;
  bool SchemeIs(const char* scheme) const {
    return value.starts_with(std::string(scheme) + "://");
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
struct NavigationEntry { bool GetIsOverridingUserAgent() const { return true; } };
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
std::string GetOhosStoreUserAgent() { return "store desktop"; }
}
blink::UserAgentMetadata StoreUserAgentMetadata() { return {false}; }
bool IsAuraShellMobilePhoneUi() { return true; }
bool IsAuraShellUserAgentPinned(content::WebContents* tab) { return tab->pinned; }
void SetAuraShellUserAgentPinned(content::WebContents* tab, bool value) { tab->pinned = value; }
blink::UserAgentOverride AuraShellDesktopSiteUserAgent() {
  return {"desktop", blink::UserAgentMetadata{false}};
}
CLASSIFIER
VIEWPORT
POLICY
STORE_POLICY
struct Watcher : content::WebContentsObserver {
  content::WebContents* contents;
  content::WebContents* web_contents() { return contents; }
  blink::UserAgentOverride before_appgallery_;
  bool before_appgallery_pinned_ = false;
  std::string appgallery_ua_;
  std::string store_ua_;
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
      assert(watcher.ApplyAppGalleryUserAgent(&h));
      assert(tab.pinned && h.overriding && !tab.inherits);
      assert(tab.ua.ua_string_override.ends_with(token));
      assert(IsAuraShellRequestingDesktopSite(&tab) == (!empty && desktop));
      assert(MobileViewportEnabled(&tab) == (empty || !desktop));
      if (!empty) assert(tab.ua.ua_metadata_override == baseline.ua_metadata_override);
      auto compatible = tab.ua;
      assert(watcher.ApplyAppGalleryUserAgent(&h)); // refresh must not save the temporary UA
      assert(tab.ua == compatible);
      h.url.value = "https://example.com/";
      assert(!watcher.ApplyAppGalleryUserAgent(&h));
      assert(tab.ua == baseline && tab.pinned == desktop);
      assert(h.overriding == !empty);
    }
  }
  content::WebContents tab;
  Watcher watcher(&tab);
  content::NavigationHandle h{{"https://appgallery.huawei.com/"}, false};
  watcher.ApplyAppGalleryUserAgent(&h);
  tab.ua = {"new desktop", blink::UserAgentMetadata{false}}; // user changes mode
  tab.pinned = true;
  watcher.ApplyAppGalleryUserAgent(&h);
  h.url.value = "https://example.com/";
  watcher.ApplyAppGalleryUserAgent(&h);
  assert(tab.ua.ua_string_override == "new desktop" && tab.pinned);
  watcher.ApplyAppGalleryUserAgent(&h);
  assert(tab.ua.ua_string_override == "new desktop");

  // Follow redirects away from either store, then start a fresh navigation.
  // Redirect callbacks must preserve the navigation's initial UA and must
  // never invoke the start-only NavigationHandle setter (which CHECKs in
  // Chromium). A fresh navigation must still restore the user's baseline.
  for (const char* origin : {"https://chromewebstore.google.com/",
                             "https://chrome.google.com/webstore/",
                             "https://appgallery.huawei.com/apptest/invite"}) {
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
print('Store UA exact hosts, metadata, mode changes, redirect lifecycle and restoration passed.')
