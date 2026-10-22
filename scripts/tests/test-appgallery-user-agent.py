#!/usr/bin/env python3
"""Compile the production AppGallery UA policy and exercise its lifecycle."""
from pathlib import Path
import os
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
classifier = function((ROOT / 'overlay/chrome/browser/ui/ohos/aura_shell_runtime_bridge.cc').read_text(),
                      'bool IsAuraShellRequestingDesktopSite(')
unit = r'''
#include <cassert>
#include <optional>
#include <string>
struct GURL {
  std::string value;
  bool SchemeIs(const char* scheme) const {
    return value.starts_with(std::string(scheme) + "://");
  }
  std::string host() const {
    auto start = value.find("://") + 3;
    return value.substr(start, value.find('/', start) - start);
  }
};
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
  GURL GetURL() { return url; }
  void SetIsOverridingUserAgent(bool value) { overriding = value; }
};
}
namespace embedder_support {
std::string GetUserAgentForOhos(bool mobile) {
  return mobile ? "Chrome/154.0.0.0 Mobile Safari/537.36 OpenHarmony/7.0"
                : "Chrome/154.0.0.0 Safari/537.36 OpenHarmony/7.0";
}
blink::UserAgentMetadata GetUserAgentMetadataForOhos(bool mobile) { return {mobile}; }
}
bool IsAuraShellMobilePhoneUi() { return true; }
bool IsAuraShellUserAgentPinned(content::WebContents* tab) { return tab->pinned; }
void SetAuraShellUserAgentPinned(content::WebContents* tab, bool value) { tab->pinned = value; }
blink::UserAgentOverride AuraShellDesktopSiteUserAgent() {
  return {"desktop", blink::UserAgentMetadata{false}};
}
CLASSIFIER
POLICY
struct Watcher {
  content::WebContents* contents;
  content::WebContents* web_contents() { return contents; }
  blink::UserAgentOverride before_appgallery_;
  bool before_appgallery_pinned_ = false;
  std::string appgallery_ua_;
METHOD
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
      Watcher watcher{&tab, {}, false, {}};
      content::NavigationHandle h{{"https://appgallery.huawei.com/apptest/invite"}, false};
      assert(watcher.ApplyAppGalleryUserAgent(&h));
      assert(tab.pinned && h.overriding && !tab.inherits);
      assert(tab.ua.ua_string_override.ends_with(token));
      assert(IsAuraShellRequestingDesktopSite(&tab) == (!empty && desktop));
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
  Watcher watcher{&tab, {}, false, {}};
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
}
'''.replace('POLICY', policy).replace('METHOD', method).replace('CLASSIFIER', classifier)
with tempfile.TemporaryDirectory(prefix='appgallery-ua-') as directory:
    cc = Path(directory) / 'policy.cc'
    exe = Path(directory) / 'policy'
    cc.write_text(unit)
    subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++20', '-Wall', '-Wextra',
                    '-Werror', str(cc), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('AppGallery exact host, additive UA, metadata, refresh, mode changes and restoration passed.')
